// src/server/transaction/TransactionManager.h
#pragma once

#include "Transaction.h"
#include "WALManager.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <thread>

namespace minisql {

class TransactionManager {
public:
    static TransactionManager& getInstance();

    // 获取当前线程的事务（读取 thread_local 状态，无实例依赖）
    static Transaction* getCurrentTransaction();

    // 设置当前线程的事务上下文
    // 用途：事务上下文实际属于「连接」而不是「线程」，
    // 工作线程处理某条连接的请求前，先把该连接的事务恢复到 thread_local
    static void setCurrentTransaction(Transaction* txn);

    // 开始事务
    Transaction* begin();

    // 提交事务
    bool commit();

    // 回滚事务
    bool rollback();

    // 初始化
    void init(const std::string& dataDir);

    // 崩溃恢复
    void recover();

    // 检查点：写CHECKPOINT记录并截断日志（数据已直接落盘）
    void checkpoint();

private:
    TransactionManager() = default;

    void removeTransaction(uint64_t txnId);
    void addTransaction(uint64_t txnId, Transaction* txn);

    // 重放单条日志
    void replayEntry(const WALEntry& entry);

    thread_local static Transaction* currentTransaction_;

    std::atomic<uint64_t> nextTxnId_{1};
    std::mutex txnMapMutex_;
    std::unordered_map<uint64_t, Transaction*> activeTransactions_;

    WALManager& wal_ = WALManager::getInstance();
};

}  // namespace minisql
