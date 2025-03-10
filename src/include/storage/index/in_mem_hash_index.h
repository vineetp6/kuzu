#pragma once

#include <memory>

#include "common/static_vector.h"
#include "common/types/internal_id_t.h"
#include "common/types/ku_string.h"
#include "common/types/types.h"
#include "storage/index/hash_index_header.h"
#include "storage/index/hash_index_slot.h"
#include "storage/index/hash_index_utils.h"
#include "storage/storage_structure/disk_array.h"
#include "storage/storage_structure/overflow_file.h"

namespace kuzu {
namespace storage {

constexpr size_t BUFFER_SIZE = 1024;
template<typename T>
using IndexBuffer = common::StaticVector<std::pair<T, common::offset_t>, BUFFER_SIZE>;
/**
 * Basic index file consists of three disk arrays: indexHeader, primary slots (pSlots), and overflow
 * slots (oSlots).
 *
 * 1. HashIndexHeader contains the current state of the hash tables (level and split information:
 * currentLevel, levelHashMask, higherLevelHashMask, nextSplitSlotId;  key data type).
 *
 * 2. Given a key, it is mapped to one of the pSlots based on its hash value and the level and
 * splitting info. The actual key and value are either stored in the pSlot, or in a chained overflow
 * slots (oSlots) of the pSlot.
 *
 * The slot data structure:
 * Each slot (p/oSlot) consists of a slot header and several entries. The max number of entries in
 * slot is given by HashIndexConstants::SLOT_CAPACITY. The size of the slot is given by
 * (sizeof(SlotHeader) + (SLOT_CAPACITY * sizeof(Entry)).
 *
 * SlotHeader: [numEntries, validityMask, nextOvfSlotId]
 * Entry: [key (fixed sized part), node_offset]
 *
 * 3. oSlots are used to store entries that comes to the designated primary slot that has already
 * been filled to the capacity. Several overflow slots can be chained after the single primary slot
 * as a singly linked link-list. Each slot's SlotHeader has information about the next overflow slot
 * in the chain and also the number of filled entries in that slot.
 *
 *  */

// T is the key type stored in the slots.
// For strings this is different than the type used when inserting/searching
// (see BufferKeyType and Key)
template<typename T>
class InMemHashIndex final {
    static_assert(getSlotCapacity<T>() <= SlotHeader::FINGERPRINT_CAPACITY);
    // Size of the validity mask
    static_assert(getSlotCapacity<T>() <= sizeof(SlotHeader().validityMask) * 8);
    static_assert(getSlotCapacity<T>() <= std::numeric_limits<entry_pos_t>::max() + 1);

public:
    explicit InMemHashIndex(OverflowFileHandle* overflowFileHandle);

    static void createEmptyIndexFiles(uint64_t indexPos, FileHandle& fileHandle);

    // Reserves space for at least the specified number of elements.
    // This reserves space for numEntries in total, regardless of existing entries in the builder
    void reserve(uint32_t numEntries);
    // Allocates the given number of new slots, ignoo
    void allocateSlots(uint32_t numSlots);

    using BufferKeyType =
        typename std::conditional<std::same_as<T, common::ku_string_t>, std::string, T>::type;
    // Appends the buffer to the index. Returns the number of values successfully inserted.
    // I.e. if a key fails to insert, its index will be the return value
    size_t append(const IndexBuffer<BufferKeyType>& buffer);
    using Key =
        typename std::conditional<std::same_as<T, common::ku_string_t>, std::string_view, T>::type;
    bool lookup(Key key, common::offset_t& result);

    uint64_t size() { return this->indexHeader.numEntries; }

    inline void clear() {
        indexHeader = HashIndexHeader();
        pSlots = std::make_unique<InMemDiskArrayBuilder<Slot<T>>>(dummy, 0, 0, true);
        oSlots = std::make_unique<InMemDiskArrayBuilder<Slot<T>>>(dummy, 0, 1, true);
    }

    struct SlotIterator {
        explicit SlotIterator(slot_id_t newSlotId, InMemHashIndex<T>* builder)
            : slotInfo{newSlotId, SlotType::PRIMARY}, slot(builder->getSlot(slotInfo)) {}
        SlotInfo slotInfo;
        Slot<T>* slot;
    };

    inline bool nextChainedSlot(SlotIterator& iter) {
        if (iter.slot->header.nextOvfSlotId != 0) {
            iter.slotInfo.slotId = iter.slot->header.nextOvfSlotId;
            iter.slotInfo.slotType = SlotType::OVF;
            iter.slot = getSlot(iter.slotInfo);
            return true;
        }
        return false;
    }

    inline uint64_t numPrimarySlots() const { return pSlots->size(); }

    inline HashIndexHeader getIndexHeader() { return indexHeader; }

private:
    // Assumes that space has already been allocated for the entry
    bool appendInternal(Key key, common::offset_t value, common::hash_t hash);
    Slot<T>* getSlot(const SlotInfo& slotInfo);

    uint32_t allocatePSlots(uint32_t numSlotsToAllocate);
    uint32_t allocateAOSlot();
    /*
     * When a slot is split, we add a new slot, which ends up with an
     * id equal to the slot to split's ID + (1 << header.currentLevel).
     * Values are then rehashed using a hash index which is one bit wider than before,
     * meaning they either stay in the existing slot, or move into the new one.
     */
    void splitSlot(HashIndexHeader& header);

    inline bool equals(Key keyToLookup, const T& keyInEntry) const {
        return keyToLookup == keyInEntry;
    }

    inline void insert(Key key, Slot<T>* slot, uint8_t entryPos, common::offset_t value,
        uint8_t fingerprint) {
        auto& entry = slot->entries[entryPos];
        entry.key = key;
        entry.value = value;
        slot->header.setEntryValid(entryPos, fingerprint);
        KU_ASSERT(HashIndexUtils::getFingerprintForHash(HashIndexUtils::hash(key)) == fingerprint);
    }
    void copy(const SlotEntry<T>& oldEntry, slot_id_t newSlotId, uint8_t fingerprint);
    void insertToNewOvfSlot(Key key, Slot<T>* previousSlot, common::offset_t offset,
        uint8_t fingerprint);
    common::hash_t hashStored(const T& key) const;

private:
    // TODO: might be more efficient to use a vector for each slot since this is now only needed
    // in-memory and it would remove the need to handle overflow slots.
    OverflowFileHandle* overflowFileHandle;
    FileHandle dummy;
    std::unique_ptr<InMemDiskArrayBuilder<Slot<T>>> pSlots;
    std::unique_ptr<InMemDiskArrayBuilder<Slot<T>>> oSlots;
    HashIndexHeader indexHeader;
};

template<>
bool InMemHashIndex<common::ku_string_t>::equals(std::string_view keyToLookup,
    const common::ku_string_t& keyInEntry) const;
} // namespace storage
} // namespace kuzu
