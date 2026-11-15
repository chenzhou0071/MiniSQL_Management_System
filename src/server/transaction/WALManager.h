// src/server/transaction/WALManager.h
#pragma once

#include "../../common/Value.h"
#include "../../common/Type.h"
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace minisql {

enum class LogType { BEGIN, INSERT, UPDATE, DELETE, COMMIT, CHECKPOINT };

struct WALEntry {
    uint64_t txnId;
    LogType type;
    std::string database;
    std::string table;
    std::string key;      // UPDATE/DELETE 记录旧行内容
    std::string value;    // INSERT/UPDATE 记录新行内容
    uint64_t lsn;
};

class WALManager {
public:
    static WALManager& getInstance();

    // 初始化WAL
    void init(const std::string& dataDir);

    // 写日志（追加到文件末尾）
    void writeLog(uint64_t txnId, LogType type,
                  const std::string& table = "",
                  const std::string& key = "",
                  const std::string& value = "",
                  const std::string& database = "");

    // 用户态缓冲 -> 内核页缓存
    void flush();

    // 内核页缓存 -> 磁盘（fsync/_commit），保证持久化
    void sync();

    // 写检查点记录并落盘
    void writeCheckpoint();

    // 读取日志
    std::vector<WALEntry> recover();

    // 清理WAL文件
    void truncate();

    // 获取WAL文件路径
    std::string getWALPath() const { return walPath_; }

    // 当前LSN
    uint64_t getCurrentLSN() const { return currentLSN_; }

    // 行内容编码/解码（长度前缀，二进制安全）
    static std::string encodeRow(const std::vector<Value>& row);
    static bool decodeRow(const std::string& data,
                          const std::vector<DataType>& types,
                          std::vector<Value>& out);

private:
    WALManager() = default;
    ~WALManager();

    WALManager(const WALManager&) = delete;
    WALManager& operator=(const WALManager&) = delete;

    uint64_t getNextLSN();

    // 调用方需持有 mutex_
    void writeEntryLocked(const WALEntry& entry);

    std::string walPath_;
    FILE* walFile_ = nullptr;
    std::mutex mutex_;
    uint64_t currentLSN_ = 0;
};

}  // namespace minisql
