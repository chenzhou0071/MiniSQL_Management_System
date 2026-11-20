// src/server/network/Session.h
#pragma once

#include <string>

namespace minisql {

class Transaction;

namespace server {

// 会话状态 - 每个TCP连接对应一个Session实例
class Session {
public:
    Session() : currentDatabase_(""), transaction_(nullptr) {}

    // 获取当前数据库名称
    const std::string& getCurrentDatabase() const {
        return currentDatabase_;
    }

    // 设置当前数据库名称
    void setCurrentDatabase(const std::string& dbName) {
        currentDatabase_ = dbName;
    }

    // 当前连接上未提交的事务（BEGIN 之后、COMMIT/ROLLBACK 之前）
    // 事务管理器用 thread_local 绑定线程，而一条连接可能被不同工作线程处理，
    // 所以把事务上下文同时挂在 Session 上，每次请求进来时恢复到当前线程
    Transaction* getTransaction() const {
        return transaction_;
    }

    void setTransaction(Transaction* txn) {
        transaction_ = txn;
    }

private:
    std::string currentDatabase_;
    Transaction* transaction_;
};

} // namespace server
} // namespace minisql
