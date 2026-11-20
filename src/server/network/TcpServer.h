// src/server/network/TcpServer.h
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>

#include "Session.h"

namespace minisql {

class Reactor;
class Transaction;

class TcpServer {
public:
    TcpServer(int port = 3306, int threadCount = 4);
    ~TcpServer();

    void start();
    void stop();

    void setDataDir(const std::string& dir) { dataDir_ = dir; }

private:
    // 会话管理（内部加锁，避免把容器元素的引用暴露到锁外）
    std::string getSessionDatabase(int fd);
    void setSessionDatabase(int fd, const std::string& db);
    Transaction* getSessionTransaction(int fd);
    void setSessionTransaction(int fd, Transaction* txn);
    void removeSession(int fd);

    int port_;
    int threadCount_;
    std::string dataDir_;
    std::unique_ptr<Reactor> reactor_;

    // 会话存储：每个 fd 对应一个 Session
    std::unordered_map<int, server::Session> sessions_;
    std::mutex sessionsMutex_;
};

}  // namespace minisql
