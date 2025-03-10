#include "storage/store/dictionary_column.h"

#include <bit>

using namespace kuzu::common;
using namespace kuzu::transaction;

namespace kuzu {
namespace storage {

using string_index_t = DictionaryChunk::string_index_t;
using string_offset_t = DictionaryChunk::string_offset_t;

DictionaryColumn::DictionaryColumn(const std::string& name, const MetadataDAHInfo& metaDAHeaderInfo,
    BMFileHandle* dataFH, BMFileHandle* metadataFH, BufferManager* bufferManager, WAL* wal,
    transaction::Transaction* transaction, RWPropertyStats stats, bool enableCompression) {
    auto dataColName = StorageUtils::getColumnName(name, StorageUtils::ColumnType::DATA, "");
    dataColumn = std::make_unique<Column>(dataColName, *LogicalType::UINT8(),
        *metaDAHeaderInfo.childrenInfos[0], dataFH, metadataFH, bufferManager, wal, transaction,
        stats, false /*enableCompression*/, false /*requireNullColumn*/);
    auto offsetColName = StorageUtils::getColumnName(name, StorageUtils::ColumnType::OFFSET, "");
    offsetColumn = std::make_unique<Column>(offsetColName, *LogicalType::UINT64(),
        *metaDAHeaderInfo.childrenInfos[1], dataFH, metadataFH, bufferManager, wal, transaction,
        stats, enableCompression, false /*requireNullColumn*/);
}

void DictionaryColumn::append(node_group_idx_t nodeGroupIdx, const DictionaryChunk& dictChunk) {
    KU_ASSERT(dictChunk.sanityCheck());
    dataColumn->append(dictChunk.getStringDataChunk(), nodeGroupIdx);
    offsetColumn->append(dictChunk.getOffsetChunk(), nodeGroupIdx);
}

void DictionaryColumn::scan(Transaction* transaction, node_group_idx_t nodeGroupIdx,
    DictionaryChunk& dictChunk) {
    auto dataMetadata = dataColumn->getMetadata(nodeGroupIdx, transaction->getType());
    // Make sure that the chunk is large enough
    auto stringDataChunk = dictChunk.getStringDataChunk();
    if (dataMetadata.numValues > stringDataChunk->getCapacity()) {
        stringDataChunk->resize(std::bit_ceil(dataMetadata.numValues));
    }
    dataColumn->scan(transaction, nodeGroupIdx, stringDataChunk);

    auto offsetMetadata = offsetColumn->getMetadata(nodeGroupIdx, transaction->getType());
    auto offsetChunk = dictChunk.getOffsetChunk();
    // Make sure that the chunk is large enough
    if (offsetMetadata.numValues > offsetChunk->getCapacity()) {
        offsetChunk->resize(std::bit_ceil(offsetMetadata.numValues));
    }
    offsetColumn->scan(transaction, nodeGroupIdx, offsetChunk);
}

void DictionaryColumn::scan(Transaction* transaction, node_group_idx_t nodeGroupIdx,
    std::vector<std::pair<string_index_t, uint64_t>>& offsetsToScan, ValueVector* resultVector,
    const ColumnChunkMetadata& indexMeta) {
    auto offsetState = offsetColumn->getReadState(transaction->getType(), nodeGroupIdx);
    auto dataState = dataColumn->getReadState(transaction->getType(), nodeGroupIdx);

    string_index_t firstOffsetToScan, lastOffsetToScan;
    auto comp = [](auto pair1, auto pair2) { return pair1.first < pair2.first; };
    auto duplicationFactor = (double)offsetState.metadata.numValues / indexMeta.numValues;
    if (duplicationFactor <= 0.5) {
        // If at least 50% of strings are duplicated, sort the offsets so we can re-use scanned
        // strings
        std::sort(offsetsToScan.begin(), offsetsToScan.end(), comp);
        firstOffsetToScan = offsetsToScan.front().first;
        lastOffsetToScan = offsetsToScan.back().first;
    } else {
        const auto& [min, max] =
            std::minmax_element(offsetsToScan.begin(), offsetsToScan.end(), comp);
        firstOffsetToScan = min->first;
        lastOffsetToScan = max->first;
    }
    // TODO(bmwinger): scan batches of adjacent values.
    // Ideally we scan values together until we reach empty pages
    // This would also let us use the same optimization for the data column,
    // where the worst case for the current method is much worse

    // Note that the list will contain duplicates when indices are duplicated.
    // Each distinct value is scanned once, and re-used when writing to each output value
    auto numOffsetsToScan = lastOffsetToScan - firstOffsetToScan + 1;
    // One extra offset to scan for the end offset of the last string
    std::vector<string_offset_t> offsets(numOffsetsToScan + 1);
    scanOffsets(transaction, offsetState, offsets.data(), firstOffsetToScan, numOffsetsToScan,
        dataState.metadata.numValues);

    for (auto pos = 0u; pos < offsetsToScan.size(); pos++) {
        auto startOffset = offsets[offsetsToScan[pos].first - firstOffsetToScan];
        auto endOffset = offsets[offsetsToScan[pos].first - firstOffsetToScan + 1];
        scanValueToVector(transaction, dataState, startOffset, endOffset, resultVector,
            offsetsToScan[pos].second);
        auto& scannedString = resultVector->getValue<ku_string_t>(offsetsToScan[pos].second);
        // For each string which has the same index in the dictionary as the one we scanned,
        // copy the scanned string to its position in the result vector
        while (pos + 1 < offsetsToScan.size() &&
               offsetsToScan[pos + 1].first == offsetsToScan[pos].first) {
            pos++;
            resultVector->setValue<ku_string_t>(offsetsToScan[pos].second, scannedString);
        }
    }
}

string_index_t DictionaryColumn::append(node_group_idx_t nodeGroupIdx, std::string_view val) {
    auto startOffset = dataColumn->appendValues(nodeGroupIdx,
        reinterpret_cast<const uint8_t*>(val.data()), nullptr /*nullChunkData*/, val.size());
    return offsetColumn->appendValues(nodeGroupIdx, reinterpret_cast<const uint8_t*>(&startOffset),
        nullptr /*nullChunkData*/, 1 /*numValues*/);
}

void DictionaryColumn::prepareCommit() {
    dataColumn->prepareCommit();
    offsetColumn->prepareCommit();
}

void DictionaryColumn::checkpointInMemory() {
    dataColumn->checkpointInMemory();
    offsetColumn->checkpointInMemory();
}

void DictionaryColumn::rollbackInMemory() {
    dataColumn->rollbackInMemory();
    offsetColumn->rollbackInMemory();
}

void DictionaryColumn::scanOffsets(Transaction* transaction, const Column::ReadState& state,
    DictionaryChunk::string_offset_t* offsets, uint64_t index, uint64_t numValues,
    uint64_t dataSize) {
    // We either need to read the next value, or store the maximum string offset at the end.
    // Otherwise we won't know what the length of the last string is.
    if (index + numValues < state.metadata.numValues) {
        offsetColumn->scan(transaction, state, index, index + numValues + 1, (uint8_t*)offsets);
    } else {
        offsetColumn->scan(transaction, state, index, index + numValues, (uint8_t*)offsets);
        offsets[numValues] = dataSize;
    }
}

void DictionaryColumn::scanValueToVector(Transaction* transaction,
    const Column::ReadState& dataState, uint64_t startOffset, uint64_t endOffset,
    ValueVector* resultVector, uint64_t offsetInVector) {
    KU_ASSERT(endOffset >= startOffset);
    // Add string to vector first and read directly into the vector
    auto& kuString =
        StringVector::reserveString(resultVector, offsetInVector, endOffset - startOffset);
    dataColumn->scan(transaction, dataState, startOffset, endOffset, (uint8_t*)kuString.getData());
    // Update prefix to match the scanned string data
    if (!ku_string_t::isShortString(kuString.len)) {
        memcpy(kuString.prefix, kuString.getData(), ku_string_t::PREFIX_LENGTH);
    }
}

bool DictionaryColumn::canCommitInPlace(Transaction* transaction, node_group_idx_t nodeGroupIdx,
    uint64_t numNewStrings, uint64_t totalStringLengthToAdd) {
    if (!canDataCommitInPlace(transaction, nodeGroupIdx, totalStringLengthToAdd)) {
        return false;
    }
    if (!canOffsetCommitInPlace(transaction, nodeGroupIdx, numNewStrings, totalStringLengthToAdd)) {
        return false;
    }
    return true;
}

bool DictionaryColumn::canDataCommitInPlace(Transaction* transaction, node_group_idx_t nodeGroupIdx,
    uint64_t totalStringLengthToAdd) {
    // Make sure there is sufficient space in the data chunk (not currently compressed)
    auto dataColumnMetadata = dataColumn->getMetadata(nodeGroupIdx, transaction->getType());
    auto totalStringDataAfterUpdate = dataColumnMetadata.numValues + totalStringLengthToAdd;
    if (totalStringDataAfterUpdate >
        dataColumnMetadata.numPages * BufferPoolConstants::PAGE_4KB_SIZE) {
        // Data cannot be updated in place
        return false;
    }
    return true;
}

bool DictionaryColumn::canOffsetCommitInPlace(Transaction* transaction,
    node_group_idx_t nodeGroupIdx, uint64_t numNewStrings, uint64_t totalStringLengthToAdd) {
    auto offsetColumnMetadata = offsetColumn->getMetadata(nodeGroupIdx, transaction->getType());
    auto dataColumnMetadata = dataColumn->getMetadata(nodeGroupIdx, transaction->getType());
    auto totalStringOffsetsAfterUpdate = dataColumnMetadata.numValues + totalStringLengthToAdd;
    auto offsetCapacity = offsetColumnMetadata.compMeta.numValues(
                              BufferPoolConstants::PAGE_4KB_SIZE, dataColumn->getDataType()) *
                          offsetColumnMetadata.numPages;
    auto numStringsAfterUpdate = offsetColumnMetadata.numValues + numNewStrings;
    if (numStringsAfterUpdate > offsetCapacity) {
        // Offsets cannot be updated in place
        return false;
    }
    // Indices are limited to 32 bits but in theory could be larger than that since the offset
    // column can grow beyond the node group size.
    //
    // E.g. one big string is written first, followed by NODE_GROUP_SIZE-1 small strings,
    // which are all updated in-place many times (which may fit if the first string is large
    // enough that 2^n minus the first string's size is large enough to fit the other strings,
    // for some n.
    // 32 bits should give plenty of space for updates.
    if (numStringsAfterUpdate > std::numeric_limits<string_index_t>::max()) [[unlikely]] {
        return false;
    }
    if (offsetColumnMetadata.compMeta.canAlwaysUpdateInPlace()) {
        return true;
    }
    if (!offsetColumnMetadata.compMeta.canUpdateInPlace(
            (const uint8_t*)&totalStringOffsetsAfterUpdate, 0,
            offsetColumn->getDataType().getPhysicalType())) {
        return false;
    }
    return true;
}

uint64_t DictionaryColumn::getNumValuesInOffsets(Transaction* transaction,
    node_group_idx_t nodeGroupIdx) {
    return offsetColumn->getMetadata(nodeGroupIdx, transaction->getType()).numValues;
}

} // namespace storage
} // namespace kuzu
