#pragma once

#include <cstdint>

#include "common/constants.h"
#include "common/copy_constructors.h"
#include "common/types/types.h"
#include "db_file_utils.h"
#include "storage/buffer_manager/bm_file_handle.h"
#include "storage/storage_utils.h"
#include "storage/wal/wal.h"
#include "transaction/transaction.h"
#include <span>

namespace kuzu {
namespace storage {

class FileHandle;

static constexpr uint64_t NUM_PAGE_IDXS_PER_PIP =
    (common::BufferPoolConstants::PAGE_4KB_SIZE - sizeof(common::page_idx_t)) /
    sizeof(common::page_idx_t);

/**
 * Header page of a disk array.
 */
struct DiskArrayHeader {
    // This constructor is needed when loading the database from file.
    DiskArrayHeader() : DiskArrayHeader(1){};

    explicit DiskArrayHeader(uint64_t elementSize);

    void saveToDisk(FileHandle& fileHandle, uint64_t headerPageIdx);

    void readFromFile(FileHandle& fileHandle, uint64_t headerPageIdx);

    bool operator==(const DiskArrayHeader& other) const = default;

    // We do not need to store numElementsPerPageLog2, elementPageOffsetMask, and numArrayPages or
    // save them on disk as they are functions of elementSize and numElements but we
    // nonetheless store them (and save them to disk) for simplicity.
    uint64_t alignedElementSizeLog2;
    uint64_t numElementsPerPageLog2;
    uint64_t elementPageOffsetMask;
    uint64_t firstPIPPageIdx;
    uint64_t numElements;
    uint64_t numAPs;
};

struct PIP {
    PIP() : nextPipPageIdx{DBFileUtils::NULL_PAGE_IDX} {}

    common::page_idx_t nextPipPageIdx;
    common::page_idx_t pageIdxs[NUM_PAGE_IDXS_PER_PIP];
};
static_assert(sizeof(PIP) == common::BufferPoolConstants::PAGE_4KB_SIZE);

struct PIPWrapper {
    PIPWrapper(FileHandle& fileHandle, common::page_idx_t pipPageIdx);

    explicit PIPWrapper(common::page_idx_t pipPageIdx) : pipPageIdx(pipPageIdx) {}

    common::page_idx_t pipPageIdx;
    PIP pipContents;
};

struct PIPUpdates {
    // Since PIPs are only appended to, the only existing PIP which may be modified is the last one
    // This gets tracked separately to make indexing into newPIPs simpler.
    std::optional<PIPWrapper> updatedLastPIP;
    std::vector<PIPWrapper> newPIPs;

    inline void clear() {
        updatedLastPIP.reset();
        newPIPs.clear();
    }
};

/**
 * DiskArray stores a disk-based array in a file. The array is broken down into a predefined and
 * stable header page, i.e., the header page of the array is always in a pre-allocated page in the
 * file. The header page contains the pointer to the first ``page indices page'' (pip). Each pip
 * stores a list of page indices that store the ``array pages''. Each PIP also stores the pageIdx of
 * the next PIP if one exists (or we use StorageConstants::NULL_PAGE_IDX as null). Array pages store
 * the actual data in the array.
 *
 * Storage structures can use multiple disk arrays in a single file by giving each one a different
 * pre-allocated stable header pageIdxs.
 *
 * We clarify the following abbreviations and conventions in the variables used in these files:
 * <ul>
 *   <li> pip: Page Indices Page
 *   <li> pipIdx: logical index of a PIP in DiskArray. For example a variable pipIdx we use with
 * value 5 indicates the 5th PIP,  not the physical disk pageIdx of where that PIP is stored.
 *   <li> pipPageIdx: the physical disk pageIdx of some PIP
 *   <li> AP: Array Page
 *   <li> apIdx: logical index of the array page in DiskArray. For example a variable apIdx with
 * value 5 indicates the 5th array page of the Disk Array (i.e., the physical offset of this would
 * correspond to the 5 element in the first PIP) not the physical disk pageIdx of an array page. <li
 *   <li> apPageIdx: the physical disk pageIdx of some PIP.
 * </ul>
 */
class BaseDiskArrayInternal {
public:
    // Used by copiers.
    BaseDiskArrayInternal(FileHandle& fileHandle, common::page_idx_t headerPageIdx,
        uint64_t elementSize);
    // Used when loading from file
    BaseDiskArrayInternal(FileHandle& fileHandle, DBFileID dbFileID,
        common::page_idx_t headerPageIdx, BufferManager* bufferManager, WAL* wal,
        transaction::Transaction* transaction, bool bypassWAL = false);

    virtual ~BaseDiskArrayInternal() = default;

    uint64_t getNumElements(
        transaction::TransactionType trxType = transaction::TransactionType::READ_ONLY);

    void get(uint64_t idx, transaction::TransactionType trxType, std::span<uint8_t> val);

    // Note: This function is to be used only by the WRITE trx.
    void update(uint64_t idx, std::span<uint8_t> val);

    // Note: This function is to be used only by the WRITE trx.
    // The return value is the idx of val in array.
    uint64_t pushBack(std::span<uint8_t> val);

    // Note: Currently, this function doesn't support shrinking the size of the array.
    uint64_t resize(uint64_t newNumElements, std::span<uint8_t> defaultVal);

    virtual inline void checkpointInMemoryIfNecessary() {
        std::unique_lock xlock{this->diskArraySharedMtx};
        checkpointOrRollbackInMemoryIfNecessaryNoLock(true /* is checkpoint */);
    }
    virtual inline void rollbackInMemoryIfNecessary() {
        std::unique_lock xlock{this->diskArraySharedMtx};
        checkpointOrRollbackInMemoryIfNecessaryNoLock(false /* is rollback */);
    }

    virtual void prepareCommit();

    // Write WriteIterator for making fast bulk changes to the disk array
    // The pages are cached while the elements are stored on the same page
    // Designed for sequential writes, but supports random writes too (at the cost that the page
    // caching is only beneficial when seeking from one element to another on the same page)
    //
    // The iterator is not locked, allowing multiple to be used at the same time, but access to
    // individual pages is locked through the BMFileHandle. It will hang if you seek/pushback on the
    // same page as another iterator in an overlapping scope.
    struct WriteIterator {
        BaseDiskArrayInternal& diskArray;
        PageCursor apCursor;
        uint32_t valueSize;
        // TODO(bmwinger): Instead of pinning the page and updating in-place, it might be better to
        // read and cache the page, then write the page to the WAL if it's ever modified. However
        // when doing bulk hashindex inserts, there's a high likelihood that every page accessed
        // will be modified, so it may be faster this way.
        WALPageIdxAndFrame walPageIdxAndFrame;
        static const transaction::TransactionType TRX_TYPE = transaction::TransactionType::WRITE;
        uint64_t idx;
        DEFAULT_MOVE_CONSTRUCT(WriteIterator);

        // Constructs WriteIterator in an invalid state. Seek must be called before accessing data
        WriteIterator(uint32_t valueSize, BaseDiskArrayInternal& diskArray)
            : diskArray(diskArray), apCursor(), valueSize(valueSize),
              walPageIdxAndFrame{common::INVALID_PAGE_IDX, common::INVALID_PAGE_IDX, nullptr},
              idx(0) {
            diskArray.hasTransactionalUpdates = true;
        }

        WriteIterator& seek(size_t newIdx);
        // Adds a new element to the disk array and seeks to the new element
        void pushBack(std::span<uint8_t> val);

        inline WriteIterator& operator+=(size_t increment) { return seek(idx + increment); }

        ~WriteIterator() { unpin(); }

        std::span<uint8_t> operator*() const {
            KU_ASSERT(idx < diskArray.headerForWriteTrx.numElements);
            KU_ASSERT(walPageIdxAndFrame.originalPageIdx != common::INVALID_PAGE_IDX);
            return std::span(walPageIdxAndFrame.frame + apCursor.elemPosInPage, valueSize);
        }

        inline uint64_t size() const { return diskArray.headerForWriteTrx.numElements; }

    private:
        void unpin();
        void getPage(common::page_idx_t newPageIdx, bool isNewlyAdded);
    };

    WriteIterator iter_mut(uint64_t valueSize);

    inline common::page_idx_t getAPIdx(uint64_t idx) const {
        return getAPIdxAndOffsetInAP(idx).pageIdx;
    }

protected:
    // Updates to new pages (new to this transaction) bypass the wal file.
    void updatePage(uint64_t pageIdx, bool isNewPage, std::function<void(uint8_t*)> updateOp);

    void updateLastPageOnDisk();

    uint64_t pushBackNoLock(std::span<uint8_t> val);

    inline uint64_t getNumElementsNoLock(transaction::TransactionType trxType) {
        return getDiskArrayHeader(trxType).numElements;
    }

    inline uint64_t getNumAPsNoLock(transaction::TransactionType trxType) {
        return getDiskArrayHeader(trxType).numAPs;
    }

    void setNextPIPPageIDxOfPIPNoLock(DiskArrayHeader* updatedDiskArrayHeader,
        uint64_t pipIdxOfPreviousPIP, common::page_idx_t nextPIPPageIdx);

    // This function does division and mod and should not be used in performance critical code.
    common::page_idx_t getAPPageIdxNoLock(common::page_idx_t apIdx,
        transaction::TransactionType trxType = transaction::TransactionType::READ_ONLY);

    // pipIdx is the idx of the PIP,  and not the physical pageIdx. This function assumes
    // that the caller has called hasPIPUpdatesNoLock and received true.
    common::page_idx_t getUpdatedPageIdxOfPipNoLock(uint64_t pipIdx);

    void clearWALPageVersionAndRemovePageFromFrameIfNecessary(common::page_idx_t pageIdx);

    virtual void checkpointOrRollbackInMemoryIfNecessaryNoLock(bool isCheckpoint);

    inline PageCursor getAPIdxAndOffsetInAP(uint64_t idx) const {
        // We assume that `numElementsPerPageLog2`, `elementPageOffsetMask`,
        // `alignedElementSizeLog2` are never modified throughout transactional updates, thus, we
        // directly use them from header here.
        common::page_idx_t apIdx = idx >> header.numElementsPerPageLog2;
        uint16_t byteOffsetInAP = (idx & header.elementPageOffsetMask)
                                  << header.alignedElementSizeLog2;
        return PageCursor{apIdx, byteOffsetInAP};
    }

private:
    bool checkOutOfBoundAccess(transaction::TransactionType trxType, uint64_t idx);
    bool hasPIPUpdatesNoLock(uint64_t pipIdx);

    inline const DiskArrayHeader& getDiskArrayHeader(transaction::TransactionType trxType) {
        if (trxType == transaction::TransactionType::READ_ONLY) {
            return header;
        } else {
            return headerForWriteTrx;
        }
    }

    // Returns the apPageIdx of the AP with idx apIdx and a bool indicating whether the apPageIdx is
    // a newly inserted page.
    std::pair<common::page_idx_t, bool> getAPPageIdxAndAddAPToPIPIfNecessaryForWriteTrxNoLock(
        DiskArrayHeader* updatedDiskArrayHeader, common::page_idx_t apIdx);

public:
    DiskArrayHeader header;

protected:
    FileHandle& fileHandle;
    DBFileID dbFileID;
    common::page_idx_t headerPageIdx;
    DiskArrayHeader headerForWriteTrx;
    bool hasTransactionalUpdates;
    BufferManager* bufferManager;
    WAL* wal;
    std::vector<PIPWrapper> pips;
    PIPUpdates pipUpdates;
    std::shared_mutex diskArraySharedMtx;
    // For write transactions only
    common::page_idx_t lastAPPageIdx;
    common::page_idx_t lastPageOnDisk;
};

template<typename U>
inline std::span<uint8_t> getSpan(U& val) {
    return std::span(reinterpret_cast<uint8_t*>(&val), sizeof(U));
}

template<typename U>
class BaseDiskArray {
    static_assert(sizeof(U) <= common::BufferPoolConstants::PAGE_4KB_SIZE);

public:
    // Used by copiers.
    BaseDiskArray(FileHandle& fileHandle, common::page_idx_t headerPageIdx, uint64_t elementSize)
        : diskArray(fileHandle, headerPageIdx, elementSize) {}
    // Used when loading from file
    // If bypassWAL is set, the buffer manager is used to pages new to this transaction to the
    // original file, but does not handle flushing them. BufferManager::flushAllDirtyPagesInFrames
    // should be called on this file handle exactly once during prepare commit.
    BaseDiskArray(FileHandle& fileHandle, DBFileID dbFileID, common::page_idx_t headerPageIdx,
        BufferManager* bufferManager, WAL* wal, transaction::Transaction* transaction,
        bool bypassWAL = false)
        : diskArray(fileHandle, dbFileID, headerPageIdx, bufferManager, wal, transaction,
              bypassWAL) {}

    // Note: This function is to be used only by the WRITE trx.
    // The return value is the idx of val in array.
    inline uint64_t pushBack(U val) { return diskArray.pushBack(getSpan(val)); }

    // Note: This function is to be used only by the WRITE trx.
    inline void update(uint64_t idx, U val) { diskArray.update(idx, getSpan(val)); }

    inline U get(uint64_t idx, transaction::TransactionType trxType) {
        U val;
        diskArray.get(idx, trxType, getSpan(val));
        return val;
    }

    // Note: Currently, this function doesn't support shrinking the size of the array.
    inline uint64_t resize(uint64_t newNumElements) {
        U defaultVal;
        return diskArray.resize(newNumElements, getSpan(defaultVal));
    }

    inline uint64_t getNumElements(
        transaction::TransactionType trxType = transaction::TransactionType::READ_ONLY) {
        return diskArray.getNumElements(trxType);
    }

    inline void checkpointInMemoryIfNecessary() { diskArray.checkpointInMemoryIfNecessary(); }
    inline void rollbackInMemoryIfNecessary() { diskArray.rollbackInMemoryIfNecessary(); }
    inline void prepareCommit() { diskArray.prepareCommit(); }

    class WriteIterator {
    public:
        explicit WriteIterator(BaseDiskArrayInternal::WriteIterator&& iter)
            : iter(std::move(iter)) {}
        inline U& operator*() { return *reinterpret_cast<U*>((*iter).data()); }
        DELETE_COPY_DEFAULT_MOVE(WriteIterator);

        inline WriteIterator& operator+=(size_t dist) {
            iter += dist;
            return *this;
        }

        inline WriteIterator& seek(size_t idx) {
            iter.seek(idx);
            return *this;
        }

        inline uint64_t idx() const { return iter.idx; }
        inline uint64_t getAPIdx() const { return iter.apCursor.pageIdx; }

        inline WriteIterator& pushBack(U val) {
            iter.pushBack(getSpan(val));
            return *this;
        }

        inline uint64_t size() const { return iter.size(); }

    private:
        BaseDiskArrayInternal::WriteIterator iter;
    };

    inline WriteIterator iter_mut() { return WriteIterator{diskArray.iter_mut(sizeof(U))}; }
    inline uint64_t getAPIdx(uint64_t idx) const { return diskArray.getAPIdx(idx); }
    uint32_t getAlignedElementSize() const { return 1 << diskArray.header.alignedElementSizeLog2; }

private:
    BaseDiskArrayInternal diskArray;
};

class BaseInMemDiskArray : public BaseDiskArrayInternal {
protected:
    BaseInMemDiskArray(FileHandle& fileHandle, common::page_idx_t headerPageIdx,
        uint64_t elementSize);
    BaseInMemDiskArray(FileHandle& fileHandle, DBFileID dbFileID, common::page_idx_t headerPageIdx,
        BufferManager* bufferManager, WAL* wal, transaction::Transaction* transaction);

public:
    // [] operator can be used to update elements, e.g., diskArray[5] = 4, when building an
    // InMemDiskArrayBuilder without transactional updates. This changes the contents directly in
    // memory and not on disk (nor on the wal).
    uint8_t* operator[](uint64_t idx);

protected:
    inline uint64_t addInMemoryArrayPage(bool setToZero) {
        inMemArrayPages.emplace_back(
            std::make_unique<uint8_t[]>(common::BufferPoolConstants::PAGE_4KB_SIZE));
        if (setToZero) {
            memset(inMemArrayPages[inMemArrayPages.size() - 1].get(), 0,
                common::BufferPoolConstants::PAGE_4KB_SIZE);
        }
        return inMemArrayPages.size() - 1;
    }

    void readArrayPageFromFile(uint64_t apIdx, common::page_idx_t apPageIdx);

    void addInMemoryArrayPageAndReadFromFile(common::page_idx_t apPageIdx);

protected:
    std::vector<std::unique_ptr<uint8_t[]>> inMemArrayPages;
};

template<typename U>
class InMemDiskArray : public BaseDiskArray<U> {
public:
    // Used when loading from file
    InMemDiskArray(FileHandle& fileHandle, DBFileID dbFileID, common::page_idx_t headerPageIdx,
        BufferManager* bufferManager, WAL* wal, transaction::Transaction* transaction)
        : BaseDiskArray<U>(fileHandle, dbFileID, headerPageIdx, bufferManager, wal, transaction) {}
    static inline common::page_idx_t addDAHPageToFile(BMFileHandle& fileHandle,
        BufferManager* bufferManager, WAL* wal) {
        DiskArrayHeader daHeader(sizeof(U));
        return DBFileUtils::insertNewPage(fileHandle, DBFileID{DBFileType::METADATA},
            *bufferManager, *wal,
            [&](uint8_t* frame) -> void { memcpy(frame, &daHeader, sizeof(DiskArrayHeader)); });
    }
};

class InMemDiskArrayBuilderInternal : public BaseInMemDiskArray {
public:
    InMemDiskArrayBuilderInternal(FileHandle& fileHandle, common::page_idx_t headerPageIdx,
        uint64_t numElements, size_t elementSize, bool setToZero = false);

    // This function is designed to be used during building of a disk array, i.e., during loading.
    // In particular, it changes the needed capacity non-transactionally, i.e., without writing
    // anything to the wal.
    void resize(uint64_t newNumElements, bool setToZero);

    void saveToDisk();

    inline uint64_t getNumElements() const { return header.numElements; }

private:
    inline uint64_t getNumArrayPagesNeededForElements(uint64_t numElements) {
        return (numElements >> this->header.numElementsPerPageLog2) +
               ((numElements & this->header.elementPageOffsetMask) > 0 ? 1 : 0);
    }
    void addNewArrayPageForBuilding();

    void setNumElementsAndAllocateDiskAPsForBuilding(uint64_t newNumElements);
};

template<typename U>
class InMemDiskArrayBuilder {
public:
    InMemDiskArrayBuilder(FileHandle& fileHandle, common::page_idx_t headerPageIdx,
        uint64_t numElements, bool setToZero = false)
        : diskArray(fileHandle, headerPageIdx, numElements, sizeof(U), setToZero) {}

    inline U& operator[](uint64_t idx) { return *(U*)diskArray[idx]; }

    inline void resize(uint64_t newNumElements, bool setToZero) {
        diskArray.resize(newNumElements, setToZero);
    }

    inline uint64_t getNumElements() const { return diskArray.getNumElements(); }
    inline uint64_t size() const { return diskArray.getNumElements(); }

    inline void saveToDisk() { diskArray.saveToDisk(); }

    uint32_t getAlignedElementSize() const { return 1 << diskArray.header.alignedElementSizeLog2; }

private:
    InMemDiskArrayBuilderInternal diskArray;
};

} // namespace storage
} // namespace kuzu
