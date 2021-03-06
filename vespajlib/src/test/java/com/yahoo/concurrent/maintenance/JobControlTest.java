// Copyright Verizon Media. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.concurrent.maintenance;

import com.yahoo.transaction.Mutex;
import org.junit.Test;

import java.time.Duration;
import java.time.Instant;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/**
 * @author bratseth
 */
public class JobControlTest {

    @Test
    public void testJobControl() {
        JobControl jobControl = new JobControl(new MockDb());

        MockMaintainer maintainer1 = new MockMaintainer();
        MockMaintainer maintainer2 = new MockMaintainer();
        assertTrue(jobControl.jobs().isEmpty());

        String job1 = "Job1";
        String job2 = "Job2";

        jobControl.started(job1, maintainer1);
        jobControl.started(job2, maintainer2);
        assertEquals(2, jobControl.jobs().size());
        assertTrue(jobControl.jobs().contains(job1));
        assertTrue(jobControl.jobs().contains(job2));

        assertTrue(jobControl.isActive(job1));
        assertTrue(jobControl.isActive(job2));

        jobControl.setActive(job1, false);
        assertFalse(jobControl.isActive(job1));
        assertTrue(jobControl.isActive(job2));

        jobControl.setActive(job2, false);
        assertFalse(jobControl.isActive(job1));
        assertFalse(jobControl.isActive(job2));

        jobControl.setActive(job1, true);
        assertTrue(jobControl.isActive(job1));
        assertFalse(jobControl.isActive(job2));

        jobControl.setActive(job2, true);
        assertTrue(jobControl.isActive(job1));
        assertTrue(jobControl.isActive(job2));

        // Run jobs on-demand
        jobControl.run(job1);
        jobControl.run(job1);
        assertEquals(2, maintainer1.maintenanceInvocations);
        jobControl.run(job2);
        assertEquals(1, maintainer2.maintenanceInvocations);

        // Running jobs on-demand ignores inactive flag
        jobControl.setActive(job1, false);
        jobControl.run(job1);
        assertEquals(3, maintainer1.maintenanceInvocations);
    }

    @Test
    public void testJobControlMayDeactivateJobs() {
        JobControl jobControl = new JobControl(new MockDb());
        MockMaintainer mockMaintainer = new MockMaintainer(jobControl);

        assertTrue(jobControl.jobs().contains("MockMaintainer"));

        assertEquals(0, mockMaintainer.maintenanceInvocations);

        mockMaintainer.run();
        assertEquals(1, mockMaintainer.maintenanceInvocations);

        jobControl.setActive("MockMaintainer", false);
        mockMaintainer.run();
        assertEquals(1, mockMaintainer.maintenanceInvocations);

        jobControl.setActive("MockMaintainer", true);
        mockMaintainer.run();
        assertEquals(2, mockMaintainer.maintenanceInvocations);
    }

    private static class MockDb implements JobControl.Db {

        private final Set<String> inactiveJobs = new HashSet<>();

        @Override
        public Set<String> readInactiveJobs() {
            return new HashSet<>(inactiveJobs);
        }

        @Override
        public void writeInactiveJobs(Set<String> inactiveJobs) {
            this.inactiveJobs.clear();
            this.inactiveJobs.addAll(inactiveJobs);
        }

        @Override
        public Mutex lockInactiveJobs() {
            return () -> {};
        }

        @Override
        public Mutex lockMaintenanceJob(String job) {
            return () -> {};
        }

    }

    private static class MockMaintainer extends Maintainer {

        int maintenanceInvocations = 0;

        private MockMaintainer(JobControl jobControl) {
            super(null, Duration.ofHours(1), Instant.now(), jobControl, List.of());
        }

        private MockMaintainer() {
            this(new JobControl(new MockDb()));
        }

        @Override
        protected void maintain() {
            maintenanceInvocations++;
        }

    }

}
