// src/server/transaction/WALManager.cpp
#include "WALManager.h"
#include "../../common/Logger.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace minisql {

// ============================================================
// WAL预写日志管理器 - 事务持久性和崩溃恢复
// ============================================================
// WAL原理：
// 在事务提交（COMMIT日志）落盘之前，不向客户端返回成功
// 数据修改会立即写入数据文件，日志用于崩溃后的重做（redo）
//
// WAL流程：
// 1. 事务开始 -> 写BEGIN日志
// 2. 执行操作 -> 修改数据文件 + 写INSERT/UPDATE/DELETE日志
// 3. 事务提交 -> 写COMMIT日志 + sync()强制落盘
// 4. 崩溃恢复 -> 重放已提交事务，忽略未提交事务
//
// 日志格式（长度前缀，二进制安全）：
// [LSN(8)][TxnID(8)][Type(1)][DbLen(4)][TableLen(4)][KeyLen(4)][ValueLen(4)]
// [Database][Table][Key][Value]
//
// 说明：
// - 每次写日志后flush()，把用户态缓冲交给内核页缓存
// - 事务提交、检查点时调用sync()，真正fsync/_commit到磁盘
// - 行内容用 encodeRow() 编码，回放时按内容匹配，避免行号重排错位
// ============================================================

namespace {

int syncFile(FILE* file) {
#ifdef _WIN32
    return _commit(_fileno(file));
#else
    return fsync(fileno(file));
#endif
}

}  // namespace

WALManager::~WALManager() {
    if (walFile_) {
        std::fflush(walFile_);
        syncFile(walFile_);
        std::fclose(walFile_);
        walFile_ = nullptr;
    }
}

WALManager& WALManager::getInstance() {
    static WALManager instance;
    return instance;
}

void WALManager::init(const std::string& dataDir) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::filesystem::path walDir = std::filesystem::path(dataDir) / "wal";
    std::error_code ec;
    std::filesystem::create_directories(walDir, ec);
    walPath_ = (walDir / "minisql.wal").string();

    if (!walFile_) {
        walFile_ = std::fopen(walPath_.c_str(), "ab");
    }
    if (!walFile_) {
        LOG_ERROR("WALManager", "Failed to open WAL file: " + walPath_);
    }
}

uint64_t WALManager::getNextLSN() {
    return ++currentLSN_;
}

void WALManager::writeEntryLocked(const WALEntry& entry) {
    if (!walFile_) {
        return;
    }

    uint8_t typeByte = static_cast<uint8_t>(entry.type);
    uint32_t dbLen = static_cast<uint32_t>(entry.database.size());
    uint32_t tableLen = static_cast<uint32_t>(entry.table.size());
    uint32_t keyLen = static_cast<uint32_t>(entry.key.size());
    uint32_t valueLen = static_cast<uint32_t>(entry.value.size());

    std::string buffer;
    buffer.reserve(sizeof(uint64_t) * 2 + 1 + sizeof(uint32_t) * 4
                   + entry.database.size() + entry.table.size()
                   + entry.key.size() + entry.value.size());

    buffer.append(reinterpret_cast<const char*>(&entry.lsn), sizeof(uint64_t));
    buffer.append(reinterpret_cast<const char*>(&entry.txnId), sizeof(uint64_t));
    buffer.append(reinterpret_cast<const char*>(&typeByte), sizeof(uint8_t));
    buffer.append(reinterpret_cast<const char*>(&dbLen), sizeof(uint32_t));
    buffer.append(reinterpret_cast<const char*>(&tableLen), sizeof(uint32_t));
    buffer.append(reinterpret_cast<const char*>(&keyLen), sizeof(uint32_t));
    buffer.append(reinterpret_cast<const char*>(&valueLen), sizeof(uint32_t));
    buffer.append(entry.database);
    buffer.append(entry.table);
    buffer.append(entry.key);
    buffer.append(entry.value);

    std::fwrite(buffer.data(), 1, buffer.size(), walFile_);
    std::fflush(walFile_);
}

void WALManager::writeLog(uint64_t txnId, LogType type,
                          const std::string& table,
                          const std::string& key,
                          const std::string& value,
                          const std::string& database) {
    std::lock_guard<std::mutex> lock(mutex_);

    WALEntry entry;
    entry.txnId = txnId;
    entry.type = type;
    entry.database = database;
    entry.table = table;
    entry.key = key;
    entry.value = value;
    entry.lsn = getNextLSN();

    writeEntryLocked(entry);
}

void WALManager::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (walFile_) {
        std::fflush(walFile_);
    }
}

void WALManager::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (walFile_) {
        std::fflush(walFile_);
        syncFile(walFile_);
    }
}

void WALManager::writeCheckpoint() {
    // 检查点之前的日志在恢复时会被跳过
    writeLog(0, LogType::CHECKPOINT);
    sync();
}

std::vector<WALEntry> WALManager::recover() {
    std::vector<WALEntry> entries;
    std::ifstream walFile(walPath_, std::ios::binary);

    if (!walFile.is_open()) {
        LOG_INFO("WALManager", "No WAL file found, skipping recovery");
        return entries;
    }

    uint64_t maxLSN = 0;

    while (walFile.peek() != EOF) {
        uint64_t lsn = 0;
        uint64_t txnId = 0;
        uint8_t typeByte = 0;
        uint32_t dbLen = 0, tableLen = 0, keyLen = 0, valueLen = 0;

        walFile.read(reinterpret_cast<char*>(&lsn), sizeof(lsn));
        walFile.read(reinterpret_cast<char*>(&txnId), sizeof(txnId));
        walFile.read(reinterpret_cast<char*>(&typeByte), sizeof(typeByte));
        walFile.read(reinterpret_cast<char*>(&dbLen), sizeof(dbLen));
        walFile.read(reinterpret_cast<char*>(&tableLen), sizeof(tableLen));
        walFile.read(reinterpret_cast<char*>(&keyLen), sizeof(keyLen));
        walFile.read(reinterpret_cast<char*>(&valueLen), sizeof(valueLen));

        // 文件尾部可能因为崩溃而截断，读到不完整的记录就停止
        if (!walFile) {
            LOG_WARN("WALManager", "Truncated WAL entry detected, stop reading");
            break;
        }

        std::string database(dbLen, '\0');
        std::string table(tableLen, '\0');
        std::string key(keyLen, '\0');
        std::string value(valueLen, '\0');

        if (dbLen > 0) walFile.read(&database[0], dbLen);
        if (tableLen > 0) walFile.read(&table[0], tableLen);
        if (keyLen > 0) walFile.read(&key[0], keyLen);
        if (valueLen > 0) walFile.read(&value[0], valueLen);

        if (!walFile) {
            LOG_WARN("WALManager", "Truncated WAL payload detected, stop reading");
            break;
        }

        entries.push_back({txnId, static_cast<LogType>(typeByte),
                           database, table, key, value, lsn});
        maxLSN = std::max(maxLSN, lsn);
    }

    // LSN必须延续，否则新日志会和旧日志重复
    {
        std::lock_guard<std::mutex> lock(mutex_);
        currentLSN_ = std::max(currentLSN_, maxLSN);
    }

    LOG_INFO("WALManager", "Recovered " + std::to_string(entries.size()) +
                           " WAL entries, last LSN=" + std::to_string(maxLSN));
    return entries;
}

void WALManager::truncate() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (walFile_) {
        std::fflush(walFile_);
        std::fclose(walFile_);
        walFile_ = nullptr;
    }

    std::remove(walPath_.c_str());

    walFile_ = std::fopen(walPath_.c_str(), "ab");
    currentLSN_ = 0;
}

// ============================================================
// 行内容编码
// 格式：[列数(4)] 每列: [isNull(1)][len(4)][内容]
// ============================================================
std::string WALManager::encodeRow(const std::vector<Value>& row) {
    std::string out;
    uint32_t columnCount = static_cast<uint32_t>(row.size());
    out.append(reinterpret_cast<const char*>(&columnCount), sizeof(uint32_t));

    for (const auto& value : row) {
        uint8_t nullFlag = value.isNull() ? 1 : 0;
        out.append(reinterpret_cast<const char*>(&nullFlag), sizeof(uint8_t));
        if (nullFlag) {
            continue;
        }

        std::string text = value.toString();
        uint32_t len = static_cast<uint32_t>(text.size());
        out.append(reinterpret_cast<const char*>(&len), sizeof(uint32_t));
        out.append(text);
    }

    return out;
}

bool WALManager::decodeRow(const std::string& data,
                           const std::vector<DataType>& types,
                           std::vector<Value>& out) {
    out.clear();
    size_t offset = 0;

    if (data.size() < sizeof(uint32_t)) {
        return false;
    }

    uint32_t columnCount = 0;
    std::memcpy(&columnCount, data.data(), sizeof(uint32_t));
    offset += sizeof(uint32_t);

    if (columnCount != types.size()) {
        return false;
    }

    for (uint32_t i = 0; i < columnCount; ++i) {
        if (offset + sizeof(uint8_t) > data.size()) {
            return false;
        }

        uint8_t nullFlag = data[offset];
        offset += sizeof(uint8_t);

        if (nullFlag) {
            out.push_back(Value(types[i]));
            continue;
        }

        if (offset + sizeof(uint32_t) > data.size()) {
            return false;
        }

        uint32_t len = 0;
        std::memcpy(&len, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        if (offset + len > data.size()) {
            return false;
        }

        std::string text = data.substr(offset, len);
        offset += len;

        try {
            out.push_back(Value::parseFrom(text, types[i]));
        } catch (...) {
            return false;
        }
    }

    return true;
}

}  // namespace minisql
