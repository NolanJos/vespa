// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "emptysearchview.h"
#include "docstorevalidator.h"
#include "document_subdb_initializer_result.h"
#include "minimal_document_retriever.h"
#include "storeonlydocsubdb.h"
#include "document_subdb_initializer.h"
#include "reconfig_params.h"
#include <vespa/searchcore/proton/attribute/attribute_writer.h>
#include <vespa/searchcore/proton/bucketdb/ibucketdbhandlerinitializer.h>
#include <vespa/searchcore/proton/docsummary/summarymanagerinitializer.h>
#include <vespa/searchcore/proton/documentmetastore/lidreusedelayer.h>
#include <vespa/searchcore/proton/documentmetastore/documentmetastoreinitializer.h>
#include <vespa/searchcore/proton/flushengine/threadedflushtarget.h>
#include <vespa/searchcore/proton/index/index_writer.h>
#include <vespa/searchcore/proton/metrics/legacy_documentdb_metrics.h>
#include <vespa/searchcore/proton/metrics/metricswireservice.h>
#include <vespa/searchlib/attribute/configconverter.h>
#include <vespa/searchlib/docstore/document_store_visitor_progress.h>
#include <vespa/searchlib/util/fileheadertk.h>
#include <vespa/vespalib/io/fileutil.h>
#include <vespa/vespalib/util/closuretask.h>
#include <vespa/vespalib/util/exceptions.h>

#include <vespa/log/log.h>
LOG_SETUP(".proton.server.storeonlydocsubdb");

using vespa::config::search::AttributesConfig;
using vespa::config::search::core::ProtonConfig;
using search::GrowStrategy;
using search::AttributeGuard;
using search::AttributeVector;
using search::IndexMetaInfo;
using search::makeLambdaTask;
using search::TuneFileDocumentDB;
using search::index::Schema;
using search::SerialNum;
using vespalib::IllegalStateException;
using vespalib::ThreadStackExecutorBase;
using proton::matching::MatchingStats;
using proton::matching::SessionManager;
using vespalib::GenericHeader;
using search::common::FileHeaderContext;
using vespalib::makeTask;
using vespalib::makeClosure;
using proton::documentmetastore::LidReuseDelayer;
using fastos::TimeStamp;
using proton::initializer::InitializerTask;

namespace proton {

namespace {

IIndexManager::SP nullIndexManager;
IIndexWriter::SP nullIndexWriter;

}

StoreOnlyDocSubDB::StoreOnlyDocSubDB(const Config &cfg, const Context &ctx)
    : DocSubDB(ctx._owner, ctx._tlSyncer),
      _docTypeName(cfg._docTypeName),
      _subName(cfg._subName),
      _baseDir(cfg._baseDir),
      _bucketDB(ctx._bucketDB),
      _bucketDBHandlerInitializer(ctx._bucketDBHandlerInitializer),
      _metaStoreCtx(),
      _attributeGrow(cfg._attributeGrow),
      _attributeGrowNumDocs(cfg._attributeGrowNumDocs),
      _flushedDocumentMetaStoreSerialNum(0u),
      _flushedDocumentStoreSerialNum(0u),
      _dms(),
      _iSummaryMgr(),
      _rSummaryMgr(),
      _summaryAdapter(),
      _writeService(ctx._writeService),
      _summaryExecutor(ctx._summaryExecutor),
      _metrics(ctx._metrics),
      _iSearchView(),
      _iFeedView(),
      _configMutex(ctx._configMutex),
      _hwInfo(ctx._hwInfo),
      _getSerialNum(ctx._getSerialNum),
      _tlsSyncer(ctx._writeService.master(), ctx._getSerialNum, ctx._tlSyncer),
      _dmsFlushTarget(),
      _subDbId(cfg._subDbId),
      _subDbType(cfg._subDbType),
      _fileHeaderContext(*this, ctx._fileHeaderContext, _docTypeName, _baseDir),
      _lidReuseDelayer(),
      _commitTimeTracker(TimeStamp::Seconds(3600.0))
{
    vespalib::mkdir(_baseDir, false); // Assume parent is created.
}

StoreOnlyDocSubDB::~StoreOnlyDocSubDB()
{
    // XXX: Disk index wrappers should not live longer than index manager
    // which owns map of active disk indexes.
    clearViews();
    // Metastore must live longer than summarystore.
    _iSummaryMgr.reset();
    _rSummaryMgr.reset();
}

size_t
StoreOnlyDocSubDB::getNumDocs() const
{
    if (_metaStoreCtx.get() != NULL) {
        return _metaStoreCtx->get().getNumUsedLids();
    } else {
        return 0u;
    }
}

size_t
StoreOnlyDocSubDB::getNumActiveDocs() const
{
    return 0;
}

bool
StoreOnlyDocSubDB::hasDocument(const document::DocumentId &id)
{
    search::DocumentIdT lid;
    IDocumentMetaStoreContext::IReadGuard::UP guard = _metaStoreCtx->getReadGuard();
    return guard->get().getLid(id.getGlobalId(), lid);
}

void
StoreOnlyDocSubDB::onReplayDone()
{
    _metaStoreCtx->get().constructFreeList();
}


void
StoreOnlyDocSubDB::onReprocessDone(SerialNum serialNum)
{
    (void) serialNum;
    _commitTimeTracker.setReplayDone();
}


SerialNum
StoreOnlyDocSubDB::getOldestFlushedSerial()
{
    SerialNum lowest(_iSummaryMgr->getBackingStore().lastSyncToken());
    lowest = std::min(lowest, _dmsFlushTarget->getFlushedSerialNum());
    return lowest;
}


SerialNum
StoreOnlyDocSubDB::getNewestFlushedSerial()
{
    SerialNum highest(_iSummaryMgr->getBackingStore().lastSyncToken());
    highest = std::max(highest, _dmsFlushTarget->getFlushedSerialNum());
    return highest;
}


initializer::InitializerTask::SP
StoreOnlyDocSubDB::
createSummaryManagerInitializer(const ProtonConfig::Summary protonSummaryCfg,
                                const search::TuneFileSummary &tuneFile,
                                search::IBucketizer::SP bucketizer,
                                std::shared_ptr<SummaryManager::SP> result) const
{
    GrowStrategy grow = _attributeGrow;
    vespalib::string baseDir(_baseDir + "/summary");
    return std::make_shared<SummaryManagerInitializer>
        (grow, baseDir, getSubDbName(), _docTypeName, _summaryExecutor,
         protonSummaryCfg,
         tuneFile,
         _fileHeaderContext, _tlSyncer, bucketizer, result);
}

void
StoreOnlyDocSubDB::setupSummaryManager(SummaryManager::SP summaryManager)
{
    _rSummaryMgr = summaryManager;
    _iSummaryMgr = _rSummaryMgr; // Upcast allowed with std::shared_ptr
    _flushedDocumentStoreSerialNum =
        _iSummaryMgr->getBackingStore().lastSyncToken();
    _summaryAdapter.reset(new SummaryAdapter(_rSummaryMgr));
}


InitializerTask::SP
StoreOnlyDocSubDB::
createDocumentMetaStoreInitializer(const search::TuneFileAttributes &tuneFile,
                                   std::shared_ptr<DocumentMetaStoreInitializerResult::SP> result) const
{
    GrowStrategy grow = _attributeGrow;
    // Amortize memory spike cost over N docs
    grow.setDocsGrowDelta(grow.getDocsGrowDelta() + _attributeGrowNumDocs);
    vespalib::string baseDir(_baseDir + "/documentmetastore");
    vespalib::string name = DocumentMetaStore::getFixedName();
    vespalib::string attrFileName = baseDir + "/" + name; // XXX: Wrong
    DocumentMetaStore::IGidCompare::SP
        gidCompare(std::make_shared<DocumentMetaStore::DefaultGidCompare>());
    // make preliminary result visible early, allowing dependent
    // initializers to get hold of document meta store instance in
    // their constructors.
    *result = std::make_shared<DocumentMetaStoreInitializerResult>
              (std::make_shared<DocumentMetaStore>(_bucketDB, attrFileName,
                                                   grow,
                                                   gidCompare, _subDbType),
               tuneFile);
    return std::make_shared<documentmetastore::DocumentMetaStoreInitializer>
        (baseDir,
         getSubDbName(),
         _docTypeName.toString(),
         (*result)->documentMetaStore());
}


void
StoreOnlyDocSubDB::setupDocumentMetaStore(DocumentMetaStoreInitializerResult::SP dmsResult)
{
    vespalib::string baseDir(_baseDir + "/documentmetastore");
    vespalib::string name = DocumentMetaStore::getFixedName();
    DocumentMetaStore::SP dms(dmsResult->documentMetaStore());
    if (dms->isLoaded()) {
        _flushedDocumentMetaStoreSerialNum =
            dms->getStatus().getLastSyncToken();
    }
    _bucketDBHandlerInitializer.
        addDocumentMetaStore(dms.get(),
                             _flushedDocumentMetaStoreSerialNum);
    _metaStoreCtx.reset(new DocumentMetaStoreContext(dms));
    LOG(debug, "Added document meta store '%s' with flushed serial num %lu",
               name.c_str(), _flushedDocumentMetaStoreSerialNum);
    _dms = dms;
    _dmsFlushTarget.reset(new DocumentMetaStoreFlushTarget(dms,
                                                           _tlsSyncer,
                                  baseDir,
                                  dmsResult->tuneFile(),
                                  _fileHeaderContext, _hwInfo));
}

DocumentSubDbInitializer::UP
StoreOnlyDocSubDB::createInitializer(const DocumentDBConfig &configSnapshot,
                                     SerialNum configSerialNum,
                                     const Schema::SP &unionSchema,
                                     const ProtonConfig::Summary &
                                     protonSummaryCfg,
                                     const ProtonConfig::Index &indexCfg) const
{
    (void) unionSchema;
    (void) configSerialNum;
    (void) indexCfg;
    auto result = std::make_unique<DocumentSubDbInitializer>
                  (const_cast<StoreOnlyDocSubDB &>(*this),
                   _writeService.master());
    auto dmsInitTask =
    createDocumentMetaStoreInitializer(configSnapshot.
                                       getTuneFileDocumentDBSP()->_attr,
                                       result->writableResult().
                                       writableDocumentMetaStore());
    result->addDocumentMetaStoreInitTask(dmsInitTask);
    auto summaryTask =
        createSummaryManagerInitializer(protonSummaryCfg,
                                        configSnapshot.
                                        getTuneFileDocumentDBSP()->_summary,
                                        result->result().documentMetaStore()->
                                        documentMetaStore(),
                                        result->writableResult().
                                        writableSummaryManager());
    result->addDependency(summaryTask);
    summaryTask->addDependency(dmsInitTask);

    LidReuseDelayerConfig lidReuseDelayerConfig(configSnapshot);
    result->writableResult().setLidReuseDelayerConfig(lidReuseDelayerConfig);
    return result;
}

void
StoreOnlyDocSubDB::setup(const DocumentSubDbInitializerResult &initResult)
{
    setupDocumentMetaStore(initResult.documentMetaStore());
    setupSummaryManager(initResult.summaryManager());
    _lidReuseDelayer.reset(new LidReuseDelayer(_writeService, *_dms));
    updateLidReuseDelayer(initResult.lidReuseDelayerConfig());
}

IFlushTarget::List
StoreOnlyDocSubDB::getFlushTargets()
{
    IFlushTarget::List ret;
    for (const auto &target : getFlushTargetsInternal()) {
        ret.push_back(IFlushTarget::SP
                (new ThreadedFlushTarget(_writeService.master(),
                                         _getSerialNum,
                                         target, _subName)));
    }
    return ret;
}

IFlushTarget::List
StoreOnlyDocSubDB::getFlushTargetsInternal()
{
    IFlushTarget::List ret(_rSummaryMgr->getFlushTargets());
    ret.push_back(_dmsFlushTarget);
    return ret;
}

StoreOnlyFeedView::Context
StoreOnlyDocSubDB::getStoreOnlyFeedViewContext(const DocumentDBConfig &configSnapshot)
{
    return StoreOnlyFeedView::Context(getSummaryAdapter(),
            configSnapshot.getSchemaSP(),
            _metaStoreCtx,
            configSnapshot.getDocumentTypeRepoSP(),
            _writeService,
            *_lidReuseDelayer, _commitTimeTracker);
}

StoreOnlyFeedView::PersistentParams
StoreOnlyDocSubDB::getFeedViewPersistentParams()
{
    SerialNum flushedDMSSN(_flushedDocumentMetaStoreSerialNum);
    SerialNum flushedDSSN(_flushedDocumentStoreSerialNum);
    return StoreOnlyFeedView::PersistentParams(flushedDMSSN,
            flushedDSSN,
            _docTypeName,
            _metrics.feed,
            _subDbId,
            _subDbType);
}

void
StoreOnlyDocSubDB::initViews(const DocumentDBConfig &configSnapshot,
                             const SessionManager::SP &sessionManager)
{
    assert(_writeService.master().isCurrentThread());
    _iSearchView.set(ISearchHandler::SP(new EmptySearchView));
    {
        std::lock_guard<std::mutex> guard(_configMutex);
        initFeedView(configSnapshot);
    }
    (void) sessionManager;
}

void
StoreOnlyDocSubDB::initFeedView(const DocumentDBConfig &configSnapshot)
{
    assert(_writeService.master().isCurrentThread());
    StoreOnlyFeedView::UP feedView(new StoreOnlyFeedView(
            getStoreOnlyFeedViewContext(configSnapshot),
            getFeedViewPersistentParams()));

    // XXX: Not exception safe.
    _iFeedView.set(StoreOnlyFeedView::SP(feedView.release()));
}

vespalib::string
StoreOnlyDocSubDB::getSubDbName() const {
    return vespalib::make_string("%s.%s", _owner.getName().c_str(), _subName.c_str());
}

void
StoreOnlyDocSubDB::updateLidReuseDelayer(const DocumentDBConfig *
                                         newConfigSnapshot)
{
    LidReuseDelayerConfig lidReuseDelayerConfig(*newConfigSnapshot);
    updateLidReuseDelayer(lidReuseDelayerConfig);
}

void
StoreOnlyDocSubDB::updateLidReuseDelayer(const LidReuseDelayerConfig &config)
{
    bool immediateCommit = config.visibilityDelay() == 0;
    /*
     * The lid reuse delayer should not have any pending lids stored at this
     * time, since DocumentDB::applyConfig() calls forceCommit() on the
     * feed view before applying the new config to the sub dbs.
     */
    _lidReuseDelayer->setImmediateCommit(immediateCommit);
    _commitTimeTracker.setVisibilityDelay(config.visibilityDelay());
}

IReprocessingTask::List
StoreOnlyDocSubDB::applyConfig(const DocumentDBConfig &newConfigSnapshot,
                               const DocumentDBConfig &oldConfigSnapshot,
                               SerialNum serialNum,
                               const ReconfigParams &params,
                               IDocumentDBReferenceResolver &resolver)
{
    (void) oldConfigSnapshot;
    (void) serialNum;
    (void) params;
    (void) resolver;
    assert(_writeService.master().isCurrentThread());
    initFeedView(newConfigSnapshot);
    updateLidReuseDelayer(&newConfigSnapshot);
    _owner.syncFeedView();
    return IReprocessingTask::List();
}

proton::IAttributeManager::SP
StoreOnlyDocSubDB::getAttributeManager() const
{
    return proton::IAttributeManager::SP();
}

const IIndexManager::SP &
StoreOnlyDocSubDB::getIndexManager() const
{
    return nullIndexManager;
}

const IIndexWriter::SP &
StoreOnlyDocSubDB::getIndexWriter() const
{
    return nullIndexWriter;
}

void
StoreOnlyDocSubDB::wipeHistory(SerialNum, const Schema &, const Schema &)
{
}

void
StoreOnlyDocSubDB::setIndexSchema(const Schema::SP &schema,
                                const Schema::SP &fusionSchema)
{
    assert(_writeService.master().isCurrentThread());
    (void) schema;
    (void) fusionSchema;
}

search::SearchableStats
StoreOnlyDocSubDB::getSearchableStats() const
{
    return search::SearchableStats();
}

IDocumentRetriever::UP
StoreOnlyDocSubDB::getDocumentRetriever()
{
    return IDocumentRetriever::UP(new MinimalDocumentRetriever(
                    _docTypeName,
                    _iFeedView.get()->getDocumentTypeRepo(),
                    *_metaStoreCtx,
                    _iSummaryMgr->getBackingStore(),
                    _subDbType != SubDbType::REMOVED));
}

MatchingStats
StoreOnlyDocSubDB::getMatcherStats(const vespalib::string &rankProfile) const
{
    (void) rankProfile;
    return MatchingStats();
}

void
StoreOnlyDocSubDB::close()
{
    assert(_writeService.master().isCurrentThread());
    search::IDocumentStore & store(_rSummaryMgr->getBackingStore());
    SerialNum syncToken = store.initFlush(store.lastSyncToken());
    _tlSyncer.sync(syncToken);
    store.flush(syncToken);
}

std::shared_ptr<IDocumentDBReference>
StoreOnlyDocSubDB::getDocumentDBReference()
{
    return std::shared_ptr<IDocumentDBReference>();
}

void
StoreOnlyDocSubDB::tearDownReferences(IDocumentDBReferenceResolver &resolver)
{
    (void) resolver;
}

void
StoreOnlySubDBFileHeaderContext::
addTags(vespalib::GenericHeader &header,
        const vespalib::string &name) const
{
    _parentFileHeaderContext.addTags(header, name);
    typedef GenericHeader::Tag Tag;
    header.putTag(Tag("documentType", _docTypeName.toString()));
    header.putTag(Tag("subDB", _subDB));
}


} // namespace proton
