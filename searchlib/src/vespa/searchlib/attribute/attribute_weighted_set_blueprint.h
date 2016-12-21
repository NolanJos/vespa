// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include <vespa/searchlib/queryeval/blueprint.h>
#include <vespa/searchlib/attribute/attributeguard.h>
#include <vespa/searchlib/queryeval/searchiterator.h>
#include <memory>
#include <vector>

namespace search {

namespace attribute { class ISearchContext; }

class AttributeWeightedSetBlueprint : public queryeval::ComplexLeafBlueprint
{
private:
    using ISearchContext = attribute::ISearchContext;
    using IAttributeVector = attribute::IAttributeVector;
    size_t                     _numDocs;
    size_t                     _estHits;
    std::vector<int32_t>       _weights;
    const IAttributeVector    & _attr;
    std::vector<ISearchContext*> _contexts;

    AttributeWeightedSetBlueprint(const AttributeWeightedSetBlueprint &); // disabled
    AttributeWeightedSetBlueprint &operator=(const AttributeWeightedSetBlueprint &); // disabled

public:
    AttributeWeightedSetBlueprint(const queryeval::FieldSpec &field, const IAttributeVector & attr);
    virtual ~AttributeWeightedSetBlueprint();
    void addToken(std::unique_ptr<ISearchContext> context, int32_t weight);
    virtual queryeval::SearchIterator::UP createLeafSearch(const fef::TermFieldMatchDataArray &tfmda, bool strict) const;

    virtual void fetchPostings(bool strict);
};

} // namespace search

