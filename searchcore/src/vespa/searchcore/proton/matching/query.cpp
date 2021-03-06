// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "query.h"
#include "blueprintbuilder.h"
#include "matchdatareservevisitor.h"
#include "resolveviewvisitor.h"
#include "termdataextractor.h"
#include "sameelementmodifier.h"
#include "unpacking_iterators_optimizer.h"
#include <vespa/document/datatype/positiondatatype.h>
#include <vespa/searchlib/common/location.h>
#include <vespa/searchlib/parsequery/stackdumpiterator.h>
#include <vespa/searchlib/query/tree/point.h>
#include <vespa/searchlib/query/tree/rectangle.h>
#include <vespa/searchlib/queryeval/intermediate_blueprints.h>
#include <vespa/searchcore/proton/documentmetastore/white_list_provider.h>

#include <vespa/log/log.h>
LOG_SETUP(".proton.matching.query");
#include <vespa/searchlib/query/tree/querytreecreator.h>

using document::PositionDataType;
using search::SimpleQueryStackDumpIterator;
using search::fef::IIndexEnvironment;
using search::fef::ITermData;
using search::fef::MatchData;
using search::fef::MatchDataLayout;
using search::fef::Location;
using search::query::Node;
using search::query::QueryTreeCreator;
using search::query::Weight;
using search::queryeval::AndBlueprint;
using search::queryeval::AndNotBlueprint;
using search::queryeval::RankBlueprint;
using search::queryeval::IntermediateBlueprint;
using search::queryeval::Blueprint;
using search::queryeval::IRequestContext;
using search::queryeval::SearchIterator;
using vespalib::string;
using std::vector;

namespace proton::matching {

namespace {

Node::UP
inject(Node::UP query, Node::UP to_inject) {
    if (auto * my_and = dynamic_cast<search::query::And *>(query.get())) {
        my_and->append(std::move(to_inject));
    } else  if (dynamic_cast<search::query::Rank *>(query.get()) || dynamic_cast<search::query::AndNot *>(query.get())) {
        search::query::Intermediate & root = static_cast<search::query::Intermediate &>(*query);
        root.prepend(inject(root.stealFirst(), std::move(to_inject)));
    } else {
        auto new_root = std::make_unique<ProtonAnd>();
        new_root->append(std::move(query));
        new_root->append(std::move(to_inject));
        query = std::move(new_root);
    }
    return query;
}

void
addLocationNode(const string &location_str, Node::UP &query_tree, Location &fef_location) {
    if (location_str.empty()) {
        return;
    }
    string::size_type pos = location_str.find(':');
    if (pos == string::npos) {
        LOG(warning, "Location string lacks attribute vector specification. loc='%s'", location_str.c_str());
        return;
    }
    const string view = PositionDataType::getZCurveFieldName(location_str.substr(0, pos));
    const string loc = location_str.substr(pos + 1);

    search::common::Location locationSpec;
    if (!locationSpec.parse(loc)) {
        LOG(warning, "Location parse error (location: '%s'): %s", location_str.c_str(), locationSpec.getParseError());
        return;
    }

    int32_t id = -1;
    Weight weight(100);

    if (locationSpec.getRankOnDistance()) {
        query_tree = inject(std::move(query_tree), std::make_unique<ProtonLocationTerm>(loc, view, id, weight));
        fef_location.setAttribute(view);
        fef_location.setXPosition(locationSpec.getX());
        fef_location.setYPosition(locationSpec.getY());
        fef_location.setXAspect(locationSpec.getXAspect());
        fef_location.setValid(true);
    } else if (locationSpec.getPruneOnDistance()) {
        query_tree = inject(std::move(query_tree), std::make_unique<ProtonLocationTerm>(loc, view, id, weight));
    }
}

IntermediateBlueprint *
asRankOrAndNot(Blueprint * blueprint) {
    IntermediateBlueprint * rankOrAndNot = dynamic_cast<RankBlueprint*>(blueprint);
    if (rankOrAndNot == nullptr) {
        rankOrAndNot = dynamic_cast<AndNotBlueprint*>(blueprint);
    }
    return rankOrAndNot;
}

IntermediateBlueprint *
lastConsequtiveRankOrAndNot(Blueprint * blueprint) {
    IntermediateBlueprint * prev = nullptr;
    IntermediateBlueprint * curr = asRankOrAndNot(blueprint);
    while (curr != nullptr) {
        prev =  curr;
        curr = asRankOrAndNot(&curr->getChild(0));
    }
    return prev;
}

}  // namespace

Query::Query() = default;
Query::~Query() = default;

bool
Query::buildTree(vespalib::stringref stack, const string &location,
                 const ViewResolver &resolver, const IIndexEnvironment &indexEnv,
                 bool split_unpacking_iterators, bool delay_unpacking_iterators)
{
    SimpleQueryStackDumpIterator stack_dump_iterator(stack);
    _query_tree = QueryTreeCreator<ProtonNodeTypes>::create(stack_dump_iterator);
    if (_query_tree) {
        SameElementModifier prefixSameElementSubIndexes;
        _query_tree->accept(prefixSameElementSubIndexes);
        addLocationNode(location, _query_tree, _location);
        _query_tree = UnpackingIteratorsOptimizer::optimize(std::move(_query_tree),
                bool(_whiteListBlueprint), split_unpacking_iterators, delay_unpacking_iterators);
        ResolveViewVisitor resolve_visitor(resolver, indexEnv);
        _query_tree->accept(resolve_visitor);
        return true;
    } else {
        // TODO(havardpe): log warning or pass message upwards
        return false;
    }
}

void
Query::extractTerms(vector<const ITermData *> &terms)
{
    TermDataExtractor::extractTerms(*_query_tree, terms);
}

void
Query::extractLocations(vector<const Location *> &locations)
{
    locations.clear();
    locations.push_back(&_location);
}

void
Query::setWhiteListBlueprint(Blueprint::UP whiteListBlueprint)
{
    _whiteListBlueprint = std::move(whiteListBlueprint);
    _white_list_provider = dynamic_cast<WhiteListProvider *>(_whiteListBlueprint.get());
}

void
Query::reserveHandles(const IRequestContext & requestContext, ISearchContext &context, MatchDataLayout &mdl)
{
    MatchDataReserveVisitor reserve_visitor(mdl);
    _query_tree->accept(reserve_visitor);

    _blueprint = BlueprintBuilder::build(requestContext, *_query_tree, context);
    LOG(debug, "original blueprint:\n%s\n", _blueprint->asString().c_str());
    if (_whiteListBlueprint) {
        auto andBlueprint = std::make_unique<AndBlueprint>();
        IntermediateBlueprint * rankOrAndNot = lastConsequtiveRankOrAndNot(_blueprint.get());
        if (rankOrAndNot != nullptr) {
            (*andBlueprint)
                    .addChild(rankOrAndNot->removeChild(0))
                    .addChild(std::move(_whiteListBlueprint));
            rankOrAndNot->insertChild(0, std::move(andBlueprint));
        } else {
            (*andBlueprint)
                    .addChild(std::move(_blueprint))
                    .addChild(std::move(_whiteListBlueprint));
            _blueprint = std::move(andBlueprint);
        }
        _blueprint->setDocIdLimit(context.getDocIdLimit());
        LOG(debug, "blueprint after white listing:\n%s\n", _blueprint->asString().c_str());
    }
}

void
Query::optimize()
{
    using search::queryeval::GlobalFilter;
    _blueprint = Blueprint::optimize(std::move(_blueprint));
    if (_blueprint->getState().want_global_filter()) {
        auto white_list = (_white_list_provider ?
                           _white_list_provider->get_white_list_filter() :
                           search::BitVector::UP());
        auto global_filter = GlobalFilter::create(std::move(white_list));
        _blueprint->set_global_filter(*global_filter);
        // optimized order may change after accounting for global filter:
        _blueprint = Blueprint::optimize(std::move(_blueprint));
    }
    LOG(debug, "optimized blueprint:\n%s\n", _blueprint->asString().c_str());
}

void
Query::fetchPostings()
{
    _blueprint->fetchPostings(search::queryeval::ExecuteInfo::create(true, 1.0));
}

void
Query::freeze()
{
    _blueprint->freeze();
}

Blueprint::HitEstimate
Query::estimate() const
{
    return _blueprint->getState().estimate();
}

SearchIterator::UP
Query::createSearch(MatchData &md) const
{
    return _blueprint->createSearch(md, true);
}

}
