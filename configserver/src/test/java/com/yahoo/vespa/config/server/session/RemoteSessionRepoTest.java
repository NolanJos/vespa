// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.vespa.config.server.session;

import com.yahoo.config.provision.TenantName;
import com.yahoo.path.Path;
import com.yahoo.text.Utf8;
import com.yahoo.vespa.config.server.TestComponentRegistry;
import com.yahoo.vespa.config.server.tenant.Tenant;
import com.yahoo.vespa.config.server.tenant.TenantRepository;
import com.yahoo.vespa.config.server.zookeeper.ConfigCurator;
import com.yahoo.vespa.curator.Curator;
import com.yahoo.vespa.curator.mock.MockCurator;
import org.junit.Before;
import org.junit.Test;

import java.time.Duration;
import java.time.Instant;
import java.util.function.LongPredicate;

import static org.hamcrest.CoreMatchers.is;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertThat;

/**
 * @author Ulf Lilleengen
 */
public class RemoteSessionRepoTest {

    private static final TenantName tenantName = TenantName.defaultName();

    private RemoteSessionRepo remoteSessionRepo;
    private Curator curator;
    TenantRepository tenantRepository;

    @Before
    public void setupFacade() {
        curator = new MockCurator();
        TestComponentRegistry componentRegistry = new TestComponentRegistry.Builder()
                .curator(curator)
                .build();
        tenantRepository = new TenantRepository(componentRegistry, false);
        tenantRepository.addTenant(tenantName);
        this.remoteSessionRepo = tenantRepository.getTenant(tenantName).getRemoteSessionRepo();
        curator.create(TenantRepository.getTenantPath(tenantName).append("/applications"));
        curator.create(TenantRepository.getSessionsPath(tenantName));
        createSession(1L, false);
        createSession(2L, false);
    }

    private void createSession(long sessionId, boolean wait) {
        createSession(sessionId, wait, tenantName);
    }

    private void createSession(long sessionId, boolean wait, TenantName tenantName) {
        Path sessionsPath = TenantRepository.getSessionsPath(tenantName);
        SessionZooKeeperClient zkc = new SessionZooKeeperClient(curator, sessionsPath.append(String.valueOf(sessionId)));
        zkc.createNewSession(Instant.now());
        if (wait) {
            Curator.CompletionWaiter waiter = zkc.getUploadWaiter();
            waiter.awaitCompletion(Duration.ofSeconds(120));
        }
    }

    @Test
    public void testInitialize() {
        assertSessionExists(1L);
        assertSessionExists(2L);
    }

    @Test
    public void testCreateSession() {
        createSession(3L, true);
        assertSessionExists(3L);
    }

    @Test
    public void testSessionStateChange() throws Exception {
        long sessionId = 3L;
        createSession(sessionId, true);
        assertSessionStatus(sessionId, Session.Status.NEW);
        assertStatusChange(sessionId, Session.Status.PREPARE);
        assertStatusChange(sessionId, Session.Status.ACTIVATE);

        Path session = TenantRepository.getSessionsPath(tenantName).append("" + sessionId);
        curator.delete(session);
        assertSessionRemoved(sessionId);
        assertNull(remoteSessionRepo.getSession(sessionId));
    }

    // If reading a session throws an exception it should be handled and not prevent other applications
    // from loading. In this test we just show that we end up with one session in remote session
    // repo even if it had bad data (by making getSessionIdForApplication() in FailingTenantApplications
    // throw an exception).
    @Test
    public void testBadApplicationRepoOnActivate() {
        long sessionId = 3L;
        TenantName mytenant = TenantName.from("mytenant");
        curator.set(TenantRepository.getApplicationsPath(mytenant).append("mytenant:appX:default"), new byte[0]); // Invalid data
        tenantRepository.addTenant(mytenant);
        Tenant tenant = tenantRepository.getTenant(mytenant);
        curator.create(TenantRepository.getSessionsPath(mytenant));
        remoteSessionRepo = tenant.getRemoteSessionRepo();
        assertThat(remoteSessionRepo.getSessions().size(), is(0));
        createSession(sessionId, true, mytenant);
        assertThat(remoteSessionRepo.getSessions().size(), is(1));
    }

    private void assertStatusChange(long sessionId, Session.Status status) throws Exception {
        Path statePath = TenantRepository.getSessionsPath(tenantName).append("" + sessionId).append(ConfigCurator.SESSIONSTATE_ZK_SUBPATH);
        curator.create(statePath);
        curator.framework().setData().forPath(statePath.getAbsolute(), Utf8.toBytes(status.toString()));
        assertSessionStatus(sessionId, status);
    }

    private void assertSessionRemoved(long sessionId) {
        waitFor(p -> remoteSessionRepo.getSession(sessionId) == null, sessionId);
        assertNull(remoteSessionRepo.getSession(sessionId));
    }

    private void assertSessionExists(long sessionId) {
        assertSessionStatus(sessionId, Session.Status.NEW);
    }

    private void assertSessionStatus(long sessionId, Session.Status status) {
        waitFor(p -> remoteSessionRepo.getSession(sessionId) != null &&
                remoteSessionRepo.getSession(sessionId).getStatus() == status, sessionId);
        assertNotNull(remoteSessionRepo.getSession(sessionId));
        assertThat(remoteSessionRepo.getSession(sessionId).getStatus(), is(status));
    }

    private void waitFor(LongPredicate predicate, long sessionId) {
        long endTime = System.currentTimeMillis() + 60_000;
        boolean ok;
        do {
            ok = predicate.test(sessionId);
            try {
                Thread.sleep(10);
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
        } while (System.currentTimeMillis() < endTime && !ok);
    }

}
