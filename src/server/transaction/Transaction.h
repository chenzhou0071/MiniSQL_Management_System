// src/server/transaction/Transaction.h
#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>

namespace minisql {

enum class OpType { INSERT, UPDATE, DELETE };

struct Operation {
    OpType type;
    std::string database;
    std::string table;
    std::string key;       // INSERT: 空; UPDATE: 旧行内容; DELETE: 被删行内容
    std::string oldValue;  // UPDATE: 旧行内容（冗余，便于直接用）
    std::string newValue;  // INSERT: 新行内容; UPDATE: 新行内容; DELETE: 空
};

class Transaction {
public:
    enum class State { ACTIVE, COMMITTED, ABORTED };

    explicit Transaction(uint64_t txnId);

    uint64_t getId() const { return txnId_; }
    State getState() const { return state_; }

    void recordOperation(OpType type,
                        const std::string& database,
                        const std::string& table,
                        const std::string& key,
                        const std::string& oldValue = "",
                        const std::string& newValue = "");

    const std::vector<Operation>& getOperations() const { return operations_; }

    void setState(State state) { state_ = state; }

private:
    uint64_t txnId_;
    State state_ = State::ACTIVE;
    std::vector<Operation> operations_;
};

}  // namespace minisql
