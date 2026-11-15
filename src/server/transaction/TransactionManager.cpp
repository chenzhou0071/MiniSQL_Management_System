// src/server/transaction/TransactionManager.cpp
#include "TransactionManager.h"
#include "../../common/Logger.h"
#include "../../storage/TableManager.h"
#include <set>

namespace minisql {

thread_local Transaction* TransactionManager::currentTransaction_ = nullptr;

TransactionManager& TransactionManager::getInstance() {
    static TransactionManager instance;
    return instance;
}

void TransactionManager::init(const std::string& dataDir) {
    wal_.init(dataDir);
    recover();
}

Transaction* TransactionManager::begin() {
    uint64_t txnId = nextTxnId_++;

    auto* txn = new Transaction(txnId);
    txn->setState(Transaction::State::ACTIVE);

    // 记录到活动事务
    addTransaction(txnId, txn);

    // 设为当前线程事务
    currentTransaction_ = txn;

    // 写WAL日志
    wal_.writeLog(txnId, LogType::BEGIN);

    LOG_INFO("TransactionManager", "Transaction " + std::to_string(txnId) + " started");

    return txn;
}

Transaction* TransactionManager::getCurrentTransaction() {
    return currentTransaction_;
}

bool TransactionManager::commit() {
    auto* txn = currentTransaction_;
    if (!txn) {
        LOG_ERROR("TransactionManager", "No active transaction to commit");
        return false;
    }

    uint64_t txnId = txn->getId();

    // 写COMMIT日志并强制落盘：日志先于"提交成功"返回
    wal_.writeLog(txnId, LogType::COMMIT);
    wal_.sync();

    txn->setState(Transaction::State::COMMITTED);

    // 从活动事务移除
    removeTransaction(txnId);

    // 清理当前线程事务
    currentTransaction_ = nullptr;

    LOG_INFO("TransactionManager", "Transaction " + std::to_string(txnId) + " committed");

    return true;
}

bool TransactionManager::rollback() {
    auto* txn = currentTransaction_;
    if (!txn) {
        LOG_ERROR("TransactionManager", "No active transaction to rollback");
        return false;
    }

    uint64_t txnId = txn->getId();

    auto& tableMgr = storage::TableManager::getInstance();

    // 从后往前撤销操作（后做的先撤销，保证依赖关系正确）
    for (auto it = txn->getOperations().rbegin();
         it != txn->getOperations().rend(); ++it) {
        const auto& op = *it;
        LOG_INFO("TransactionManager",
                 "Rolling back: " + std::to_string(static_cast<int>(op.type)) +
                 " on " + op.database + "." + op.table);

        // 获取表定义（拿列类型用于解码行内容）
        auto tableDefResult = tableMgr.getTableDef(op.database, op.table);
        if (!tableDefResult.isSuccess()) {
            LOG_WARN("TransactionManager",
                     "Skip undo, table not found: " + op.database + "." + op.table);
            continue;
        }
        TableDef tableDef = *tableDefResult.getValue();

        std::vector<DataType> types;
        types.reserve(tableDef.columns.size());
        for (const auto& column : tableDef.columns) {
            types.push_back(column.type);
        }

        // 加载表数据
        auto tableDataResult = tableMgr.loadTable(op.database, op.table);
        if (!tableDataResult.isSuccess()) {
            LOG_WARN("TransactionManager",
                     "Skip undo, cannot load table: " + op.database + "." + op.table);
            continue;
        }
        storage::TableData tableData = *tableDataResult.getValue();

        // 按行内容匹配，避免行号因删除重排而错位
        auto findByContent = [&tableData](const Row& target, int& rowId) -> bool {
            for (const auto& pair : tableData) {
                if (pair.second == target) {
                    rowId = pair.first;
                    return true;
                }
            }
            return false;
        };

        switch (op.type) {
            case OpType::INSERT: {
                // 撤销 INSERT：按新行内容找到该行并删除
                Row newRow;
                if (!WALManager::decodeRow(op.newValue, types, newRow)) {
                    LOG_WARN("TransactionManager", "Undo INSERT: decode row failed");
                    break;
                }
                int rowId = -1;
                if (findByContent(newRow, rowId)) {
                    tableMgr.deleteRow(op.database, op.table, rowId);
                    LOG_INFO("TransactionManager",
                             "Undo INSERT: deleted row " + std::to_string(rowId));
                }
                break;
            }

            case OpType::UPDATE: {
                // 撤销 UPDATE：按新行内容找到该行，用旧行覆盖
                Row oldRow, newRow;
                if (!WALManager::decodeRow(op.oldValue, types, oldRow) ||
                    !WALManager::decodeRow(op.newValue, types, newRow)) {
                    LOG_WARN("TransactionManager", "Undo UPDATE: decode row failed");
                    break;
                }
                int rowId = -1;
                if (findByContent(newRow, rowId)) {
                    tableMgr.updateRow(op.database, op.table, rowId, oldRow);
                    LOG_INFO("TransactionManager",
                             "Undo UPDATE: restored row " + std::to_string(rowId));
                }
                break;
            }

            case OpType::DELETE: {
                // 撤销 DELETE：把被删行插回去
                Row deletedRow;
                if (!WALManager::decodeRow(op.oldValue, types, deletedRow)) {
                    LOG_WARN("TransactionManager", "Undo DELETE: decode row failed");
                    break;
                }
                tableMgr.insertRow(op.database, op.table, deletedRow);
                LOG_INFO("TransactionManager", "Undo DELETE: restored row");
                break;
            }

            default:
                break;
        }
    }

    txn->setState(Transaction::State::ABORTED);

    // 从活动事务移除
    removeTransaction(txnId);

    // 清理当前线程事务
    currentTransaction_ = nullptr;

    LOG_INFO("TransactionManager", "Transaction " + std::to_string(txnId) + " rolled back");

    return true;
}

void TransactionManager::addTransaction(uint64_t txnId, Transaction* txn) {
    std::lock_guard<std::mutex> lock(txnMapMutex_);
    activeTransactions_[txnId] = txn;
}

void TransactionManager::removeTransaction(uint64_t txnId) {
    std::lock_guard<std::mutex> lock(txnMapMutex_);
    activeTransactions_.erase(txnId);
}

// ============================================================
// 崩溃恢复
// 1. 从最后一个检查点开始扫描日志
// 2. 收集已提交事务（出现COMMIT记录；txnId==0 视为自动提交）
// 3. 按LSN顺序重放已提交事务的写操作（redo）
// 4. 未提交事务直接跳过（相当于undo，当前数据层没有未提交脏页）
// 5. 恢复完成后写检查点并截断日志
// ============================================================
void TransactionManager::recover() {
    auto entries = wal_.recover();
    if (entries.empty()) {
        LOG_INFO("TransactionManager", "Nothing to recover");
        return;
    }

    // 1. 定位最后一个检查点
    size_t start = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].type == LogType::CHECKPOINT) {
            start = i + 1;
        }
    }

    // 2. 收集已提交事务
    std::set<uint64_t> committed;
    for (size_t i = start; i < entries.size(); ++i) {
        if (entries[i].type == LogType::COMMIT) {
            committed.insert(entries[i].txnId);
        }
    }

    // 3. 重放
    int replayed = 0;
    int skipped = 0;
    for (size_t i = start; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (entry.type != LogType::INSERT &&
            entry.type != LogType::UPDATE &&
            entry.type != LogType::DELETE) {
            continue;
        }

        // txnId == 0 表示自动提交（DML未显式开启事务）
        bool isAutoCommit = (entry.txnId == 0);
        bool isCommitted = committed.find(entry.txnId) != committed.end();

        if (!isAutoCommit && !isCommitted) {
            ++skipped;
            continue;
        }

        replayEntry(entry);
        ++replayed;
    }

    LOG_INFO("TransactionManager",
             "Recovery finished: replayed " + std::to_string(replayed) +
             " entries, skipped " + std::to_string(skipped) + " uncommitted");

    // 4. 数据已落盘，写检查点并截断日志
    checkpoint();
}

void TransactionManager::replayEntry(const WALEntry& entry) {
    if (entry.database.empty() || entry.table.empty()) {
        return;
    }

    auto& tableMgr = storage::TableManager::getInstance();

    auto tableDefResult = tableMgr.getTableDef(entry.database, entry.table);
    if (!tableDefResult.isSuccess()) {
        LOG_WARN("TransactionManager",
                 "Skip replay, table not found: " + entry.database + "." + entry.table);
        return;
    }
    TableDef tableDef = *tableDefResult.getValue();

    std::vector<DataType> types;
    types.reserve(tableDef.columns.size());
    for (const auto& column : tableDef.columns) {
        types.push_back(column.type);
    }

    auto tableDataResult = tableMgr.loadTable(entry.database, entry.table);
    if (!tableDataResult.isSuccess()) {
        return;
    }
    storage::TableData tableData = *tableDataResult.getValue();

    // 按行内容匹配，避免行号因删除重排而错位
    auto findByContent = [&tableData](const Row& target, int& rowId) -> bool {
        for (const auto& pair : tableData) {
            if (pair.second == target) {
                rowId = pair.first;
                return true;
            }
        }
        return false;
    };

    switch (entry.type) {
        case LogType::INSERT: {
            Row newRow;
            if (!WALManager::decodeRow(entry.value, types, newRow)) {
                return;
            }

            int existingId = -1;
            if (findByContent(newRow, existingId)) {
                return;  // 已经存在，保证重放幂等
            }

            tableMgr.insertRow(entry.database, entry.table, newRow);
            break;
        }

        case LogType::UPDATE: {
            Row oldRow;
            Row newRow;
            if (!WALManager::decodeRow(entry.key, types, oldRow) ||
                !WALManager::decodeRow(entry.value, types, newRow)) {
                return;
            }

            int rowId = -1;
            if (findByContent(oldRow, rowId)) {
                tableMgr.updateRow(entry.database, entry.table, rowId, newRow);
            }
            break;
        }

        case LogType::DELETE: {
            Row oldRow;
            if (!WALManager::decodeRow(entry.key, types, oldRow)) {
                return;
            }

            int rowId = -1;
            if (findByContent(oldRow, rowId)) {
                tableMgr.deleteRow(entry.database, entry.table, rowId);
            }
            break;
        }

        default:
            break;
    }
}

void TransactionManager::checkpoint() {
    wal_.writeCheckpoint();
    wal_.truncate();
    LOG_INFO("TransactionManager", "Checkpoint completed, WAL truncated");
}

}  // namespace minisql
