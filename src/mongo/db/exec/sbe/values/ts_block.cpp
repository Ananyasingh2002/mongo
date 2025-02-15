/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/exec/sbe/values/ts_block.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <tuple>
#include <utility>

#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/bson_block.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/scalar_mono_cell_block.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/util/itoa.h"

namespace mongo::sbe::value {

TsBucketPathExtractor::TsBucketPathExtractor(std::vector<CellBlock::PathRequest> pathReqs,
                                             StringData timeField)
    : _pathReqs(std::move(pathReqs)), _timeField(timeField) {

    size_t idx = 0;
    for (auto& req : _pathReqs) {
        tassert(7796405,
                "Path must start with a Get operation",
                std::holds_alternative<CellBlock::Get>(req.path[0]));

        StringData field = std::get<CellBlock::Get>(req.path[0]).field;
        _topLevelFieldToIdxes[field].push_back(idx);

        if (req.path.size() > 2) {
            _nonTopLevelGetPathIdxes.insert(idx);
        }

        ++idx;
    }
}

std::vector<std::unique_ptr<CellBlock>> TsBucketPathExtractor::extractCellBlocks(
    const BSONObj& bucketObj) {

    BSONElement bucketControl = bucketObj[timeseries::kBucketControlFieldName];
    invariant(!bucketControl.eoo());


    const int noOfMeasurements = [&]() {
        if (auto ct = bucketControl.Obj()[timeseries::kBucketControlCountFieldName]) {
            return static_cast<int>(ct.numberLong());
        }
        return timeseries::BucketUnpacker::computeMeasurementCount(bucketObj,
                                                                   StringData(_timeField));
    }();

    const BSONElement bucketDataElem = bucketObj[timeseries::kBucketDataFieldName];
    invariant(!bucketDataElem.eoo());
    invariant(bucketDataElem.type() == BSONType::Object);

    // Build a mapping from the top level field name to the bucket's corresponding bson element.
    StringMap<BSONElement> topLevelFieldToBsonElt;
    for (auto elt : bucketDataElem.embeddedObject()) {
        auto it = _topLevelFieldToIdxes.find(elt.fieldNameStringData());
        if (it != _topLevelFieldToIdxes.end()) {
            auto [blockTag, blockVal] = bson::convertFrom<true>(elt);
            tassert(7796400,
                    "Unsupported type for timeseries bucket data",
                    blockTag == value::TypeTags::bsonObject ||
                        blockTag == value::TypeTags::bsonBinData);
            topLevelFieldToBsonElt[elt.fieldName()] = elt;
        }
    }

    std::vector<std::unique_ptr<CellBlock>> out(_pathReqs.size());

    // The time series decoding API gives us the top level fields only, and our CellBlock
    // extraction code expects full BSON objects. For now we resolve this mismatch by converting
    // the decoded output into BSON, and then re-extracting. This is really awful in terms of
    // performance, but the hope is that a new decoding API will be made available, and this
    // code can be deleted.

    // To avoid repeated allocations, we put all of the BSONObjs into one giant buffer (bsonBuffer).
    // We keep track of their offsets in 'bsonOffsets'.
    BufBuilder bsonBuffer;
    std::vector<BSONObjBuilder> bsonBuilders;
    std::vector<size_t> bsonOffsets;
    std::vector<BSONObj> bsons;

    bsonBuilders.reserve(noOfMeasurements);
    bsonOffsets.reserve(noOfMeasurements);
    bsons.reserve(noOfMeasurements);

    for (auto& [topLevelField, columnElt] : topLevelFieldToBsonElt) {
        // The set of indexes in _pathReqs which begin with this top level field.
        const auto& pathIndexesForCurrentField = _topLevelFieldToIdxes[topLevelField];
        auto [columnTag, columnVal] = bson::convertFrom<true>(columnElt);

        // Initialize a TsCellBlockForTopLevelField for the top level field. For paths of the form
        // [Get <field> Id], or equivalent, we will simply hand them this CellBlock. For nested
        // paths, we will call extract() on this CellBlock, and then pull the values out from the
        // nested bson.
        auto topLevelCellBlockUniquePtr = std::make_unique<value::TsCellBlockForTopLevelField>(
            noOfMeasurements, /*owned*/ false, columnTag, columnVal);
        auto* topLevelCellBlock = topLevelCellBlockUniquePtr.get();

        // Build a list of values 'pathIndexesForCurrentField' which are not top-level Gets.
        std::vector<size_t> nonTopLevelIdxesForCurrentField;

        for (auto idx : pathIndexesForCurrentField) {
            if (_nonTopLevelGetPathIdxes.count(idx) == 0) {
                // This path is a top level [Get <field> Id] path. We assign to its corresponding
                // output the top level cellblock. Note that we keep the raw pointer to this
                // CellBlock in 'topLevelCellBlock' so that if we end up decoding this CellBlock,
                // we do so once, and via same TsCellBlockForTopLevelField instance.
                invariant(topLevelCellBlockUniquePtr);
                out[idx] = std::move(topLevelCellBlockUniquePtr);
            } else {
                // Remember this PathReq index for later.
                nonTopLevelIdxesForCurrentField.push_back(idx);
            }
        }

        // There are no more paths that were requested which begin with this top level field.
        if (nonTopLevelIdxesForCurrentField.empty()) {
            continue;
        }

        auto extracted = topLevelCellBlock->getValueBlock().extract();
        invariant(extracted.count == static_cast<size_t>(noOfMeasurements));

        // First check if we are traversing a top level field AND there are no arrays. The path
        // must look like: [Get <field> Traverse Id]. If this is the case, we take a fast path and
        // skip the work of shredding the whole thing.

        bool allUsedFastPath = true;
        for (auto pathIdx : nonTopLevelIdxesForCurrentField) {
            if (_pathReqs[pathIdx].path.size() == 3 &&
                std::holds_alternative<CellBlock::Get>(_pathReqs[pathIdx].path[0]) &&
                std::holds_alternative<CellBlock::Traverse>(_pathReqs[pathIdx].path[1]) &&
                std::holds_alternative<CellBlock::Id>(_pathReqs[pathIdx].path[2]) &&
                // TODO: In the future could use the bucket metadata to speed up this check and
                // prove that no elements are arrays. Then we wouldn't actually have to call
                // extract() yet.
                std::none_of(extracted.tags, extracted.tags + extracted.count, isArray)) {
                // In this case the top level TsCellBlockForTopLevelField (representing the [Get
                // <field> Id]) is identical to the path [Get <field> Traverse Id]. We simply copy
                // the cell block for the [Get <field> Id] path.
                out[pathIdx] = topLevelCellBlock->clone();
            } else {
                allUsedFastPath = false;
            }
        }

        if (allUsedFastPath) {
            // There's no need to do any more work for this top level field. Every path request
            // was a top level get or eligible for the fast path.
            continue;
        }

        for (size_t i = 0; i < extracted.count; ++i) {
            bsonOffsets.push_back(bsonBuffer.len());
            bsonBuilders.push_back(BSONObjBuilder(bsonBuffer));
            bson::appendValueToBsonObj(bsonBuilders.back(),
                                       columnElt.fieldNameStringData(),
                                       extracted[i].first,
                                       extracted[i].second);
            bsonBuilders.back().doneFast();
        }

        for (size_t i = 0; i < extracted.count; ++i) {
            bsons.push_back(BSONObj(bsonBuffer.buf() + bsonOffsets[i]));
        }

        std::vector<CellBlock::PathRequest> reqs;
        for (auto idx : nonTopLevelIdxesForCurrentField) {
            reqs.push_back(_pathReqs[idx]);
        }
        auto extractedCellBlocks = value::extractCellBlocksFromBsons(reqs, bsons);
        invariant(reqs.size() == extractedCellBlocks.size());

        for (size_t i = 0; i < extractedCellBlocks.size(); ++i) {
            out[nonTopLevelIdxesForCurrentField[i]] = std::move(extractedCellBlocks[i]);
        }

        bsonBuilders.clear();
        bsonOffsets.clear();
        bsons.clear();
        bsonBuffer.reset();
    }

    // Fill in any empty spots in the output with a block of [Nothing, Nothing...].
    for (auto& cellBlock : out) {
        if (!cellBlock) {
            auto emptyBlock = std::make_unique<value::ScalarMonoCellBlock>(
                noOfMeasurements, value::TypeTags::Nothing, value::Value(0));
            cellBlock = std::move(emptyBlock);
        }
    }
    return out;
}

TsBlock::TsBlock(size_t ncells, bool owned, TypeTags blockTag, Value blockVal)
    : _blockOwned(owned), _blockTag(blockTag), _blockVal(blockVal), _count(ncells) {
    invariant(_blockTag == TypeTags::bsonObject || _blockTag == TypeTags::bsonBinData);
}

TsBlock::~TsBlock() {
    if (_blockOwned) {
        // The underlying buffer is owned by this TsBlock and so this releases it.
        releaseValue(_blockTag, _blockVal);
    }
}

void TsBlock::deblockFromBsonObj(std::vector<TypeTags>& deblockedTags,
                                 std::vector<Value>& deblockedVals) const {
    ObjectEnumerator enumerator(TypeTags::bsonObject, _blockVal);
    for (size_t i = 0; i < _count; ++i) {
        auto [tag, val] = [&] {
            if (enumerator.atEnd() || ItoA(i) != enumerator.getFieldName()) {
                // There's a missing index or a hole in the middle or at the tail, so returns
                // Nothing.
                return std::make_pair(TypeTags::Nothing, Value(0));
            } else {
                auto tagVal = enumerator.getViewOfValue();
                enumerator.advance();
                // Always makes a copy to match the behavior to the BSONColumn case's and simplify
                // the SBE value ownership model. The underlying buffer for the BSON object block is
                // owned by this TsBlock or not so we would not necessarily need to always copy the
                // values out of it.
                //
                // TODO SERVER-79612: Avoid copying values out of the BSON object block if
                // necessary.
                return copyValue(tagVal.first, tagVal.second);
            }
        }();

        ValueGuard guard(tag, val);
        deblockedTags.push_back(tag);
        deblockedVals.push_back(val);
        guard.reset();
    }
}

void TsBlock::deblockFromBsonColumn(std::vector<TypeTags>& deblockedTags,
                                    std::vector<Value>& deblockedVals) const {
    tassert(7796401,
            "Invalid BinDataType for BSONColumn",
            getBSONBinDataSubtype(TypeTags::bsonBinData, _blockVal) == BinDataType::Column);
    BSONColumn blockColumn(
        BSONBinData{value::getBSONBinData(TypeTags::bsonBinData, _blockVal),
                    static_cast<int>(value::getBSONBinDataSize(TypeTags::bsonBinData, _blockVal)),
                    BinDataType::Column});
    auto it = blockColumn.begin();
    for (size_t i = 0; i < _count; ++i) {
        // BSONColumn::Iterator decompresses values into its own buffer which is invalidated
        // whenever the iterator advances, so we need to copy them out.
        auto [tag, val] = bson::convertFrom</*View*/ false>(*it);
        ++it;

        ValueGuard guard(tag, val);
        deblockedTags.push_back(tag);
        deblockedVals.push_back(val);
        guard.reset();
    }
}

std::unique_ptr<TsBlock> TsBlock::cloneStrongTyped() const {
    // TODO: If we've already decoded the output, there's no need to re-copy the entire bson
    // column. We could instead just copy the decoded values and metadata.

    auto [cpyTag, cpyVal] = copyValue(_blockTag, _blockVal);
    ValueGuard guard(cpyTag, cpyVal);
    // The new copy must own the copied underlying buffer.
    auto cpy = std::make_unique<TsBlock>(_count, /*owned*/ true, cpyTag, cpyVal);
    guard.reset();

    // If the block has been deblocked, then we need to copy the deblocked values too to
    // avoid deblocking overhead again.
    cpy->_deblockedStorage = _deblockedStorage;

    return cpy;
}

std::unique_ptr<ValueBlock> TsBlock::clone() const {
    return std::unique_ptr<ValueBlock>(cloneStrongTyped().release());
}

DeblockedTagVals TsBlock::deblock(boost::optional<DeblockedTagValStorage>& storage) const {
    ensureDeblocked(storage);

    return DeblockedTagVals{storage->vals.size(), storage->tags.data(), storage->vals.data()};
}

void TsBlock::ensureDeblocked(boost::optional<DeblockedTagValStorage>& storage) const {
    if (!storage) {
        storage = DeblockedTagValStorage{};

        storage->owned = true;
        storage->tags.reserve(_count);
        storage->vals.reserve(_count);

        if (_blockTag == TypeTags::bsonObject) {
            deblockFromBsonObj(storage->tags, storage->vals);
        } else {
            deblockFromBsonColumn(storage->tags, storage->vals);
        }
    }
}


ValueBlock& TsCellBlockForTopLevelField::getValueBlock() {
    return *_tsBlock;
}

std::unique_ptr<CellBlock> TsCellBlockForTopLevelField::clone() const {
    auto precomputedCount = _tsBlock->tryCount();
    tassert(
        7943900, "Assumes count() is available in O(1) time on TS Block type", precomputedCount);
    auto tsBlockClone = _tsBlock->cloneStrongTyped();

    // Using raw new to access private constructor.
    return std::unique_ptr<TsCellBlockForTopLevelField>(
        new TsCellBlockForTopLevelField(*precomputedCount, std::move(tsBlockClone)));
}

TsCellBlockForTopLevelField::TsCellBlockForTopLevelField(size_t count,
                                                         bool owned,
                                                         TypeTags topLevelTag,
                                                         Value topLevelVal)
    : TsCellBlockForTopLevelField(
          count,
          // The 'count' means the number of cells in this TsCellBlockForTopLevelField and as of
          // now, we only support top-level fields only, the number of values per cell is always 1
          // and the number of cells in this TsCellBlockForTopLevelField is always the same as the
          // number of values in '_tsBlock'. So, we pass 'count' to '_tsBlock' as the number of
          // values in it.
          std::make_unique<TsBlock>(count, owned, topLevelTag, topLevelVal)) {}

TsCellBlockForTopLevelField::TsCellBlockForTopLevelField(size_t count,
                                                         std::unique_ptr<TsBlock> tsBlock)
    : _tsBlock(std::move(tsBlock)) {
    _positionInfo.resize(count, char(1));
}
}  // namespace mongo::sbe::value
