#pragma once

#include <filesystem>

#include "common/string_format.h"
#include "gtest/gtest.h"
#include "main/kuzu.h"
#include "test_helper/test_helper.h"

using ::testing::Test;

namespace kuzu {
namespace testing {

enum class TransactionTestType : uint8_t {
    NORMAL_EXECUTION = 0,
    RECOVERY = 1,
};

static void removeDir(const std::string& dir) {
    if (std::filesystem::exists(dir)) {
        std::error_code removeErrorCode;
        if (!std::filesystem::remove_all(dir, removeErrorCode)) {
            throw common::Exception(common::stringFormat(
                "Error removing directory {}.  Error Message: {}", dir, removeErrorCode.message()));
        }
    }
}

class BaseGraphTest : public Test {
public:
    void SetUp() override {
        systemConfig = std::make_unique<main::SystemConfig>(
            common::BufferPoolConstants::DEFAULT_BUFFER_POOL_SIZE_FOR_TESTING,
            2 /*numThreadsForExec*/);
        setDatabasePath();
        removeDir(databasePath);

        ieDBPath = "";
    }

    virtual std::string getInputDir() = 0;

    void TearDown() override { removeDir(databasePath); }

    void removeIEDBPath() const {
        if (ieDBPath != "") {
            auto lastSlashPos = ieDBPath.rfind('/');
            auto deletePath = ieDBPath.substr(0, lastSlashPos);
            removeDir(deletePath);
        }
    }

    void createDBAndConn();

    // multiple conns test
    void createDB();
    void createConns(const std::set<std::string>& connNames);

    void initGraph();

    void commitOrRollbackConnection(bool isCommit, TransactionTestType transactionTestType) const;

    void setIEDatabasePath(std::string filePath) { ieDBPath = filePath; }

protected:
    // Static functions to access Database's non-public properties/interfaces.
    static inline catalog::Catalog* getCatalog(main::Database& database) {
        return database.catalog.get();
    }
    static inline storage::StorageManager* getStorageManager(main::Database& database) {
        return database.storageManager.get();
    }
    static inline storage::BufferManager* getBufferManager(main::Database& database) {
        return database.bufferManager.get();
    }
    static inline storage::MemoryManager* getMemoryManager(main::Database& database) {
        return database.memoryManager.get();
    }
    static inline common::VirtualFileSystem* getFileSystem(main::Database& database) {
        return database.vfs.get();
    }
    static inline uint64_t getBMSize(main::Database& database) {
        return database.systemConfig.bufferPoolSize;
    }
    static inline storage::WAL* getWAL(main::Database& database) { return database.wal.get(); }
    static inline void commitAndCheckpointOrRollback(main::Database& database,
        transaction::Transaction* writeTransaction, bool isCommit,
        bool skipCheckpointForTestingRecovery = false) {
        if (isCommit) {
            database.commit(writeTransaction, skipCheckpointForTestingRecovery);
        } else {
            database.rollback(writeTransaction, skipCheckpointForTestingRecovery);
        }
    }
    static inline processor::QueryProcessor* getQueryProcessor(main::Database& database) {
        return database.queryProcessor.get();
    }

    // Static functions to access Connection's non-public properties/interfaces.
    static inline main::ClientContext* getClientContext(main::Connection& connection) {
        return connection.clientContext.get();
    }
    static inline void sortAndCheckTestResults(std::vector<std::string>& actualResult,
        std::vector<std::string>& expectedResult) {
        sort(expectedResult.begin(), expectedResult.end());
        ASSERT_EQ(actualResult, expectedResult);
    }
    static inline bool containsOverflowFile(common::LogicalTypeID typeID) {
        return typeID == common::LogicalTypeID::STRING || typeID == common::LogicalTypeID::LIST;
    }

    void commitOrRollbackConnectionAndInitDBIfNecessary(bool isCommit,
        TransactionTestType transactionTestType);

    inline std::string getTestGroupAndName() {
        const ::testing::TestInfo* const testInfo =
            ::testing::UnitTest::GetInstance()->current_test_info();
        return std::string(testInfo->test_case_name()) + "." + std::string(testInfo->name());
    }

private:
    void setDatabasePath() { databasePath = TestHelper::getTempDir(getTestGroupAndName()); }

public:
    std::string databasePath;
    std::unique_ptr<main::SystemConfig> systemConfig;
    std::unique_ptr<main::Database> database;
    // for normal conn; if it is null, respresents multiple conns, need to use connMap
    std::unique_ptr<main::Connection> conn;
    // for multiple conns
    std::unordered_map<std::string, std::unique_ptr<main::Connection>> connMap;
    // for export import db
    std::string ieDBPath;
};

} // namespace testing
} // namespace kuzu
