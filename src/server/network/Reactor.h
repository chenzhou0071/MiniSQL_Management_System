// src/server/network/Reactor.h
#pragma once

#include <sys/epoll.h>
#include <memory>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <unordered_map>

namespace minisql {

class TcpConnection;
class ThreadPool;

class Reactor {
public:
    Reactor(int port, int threadCount = 4);
    ~Reactor();

    void start();
    void stop();

    // 处理客户端连接
    void handleNewConnection();

    // 处理读事件
    void handleRead(int fd);

    // 设置SQL处理函数
    void setSqlHandler(std::function<void(int fd, const std::string& sql)> handler) {
        sqlHandler_ = handler;
    }

    // 设置连接关闭回调（用于清理会话、回滚未提交事务）
    void setCloseHandler(std::function<void(int fd)> handler) {
        closeHandler_ = handler;
    }

private:
    void eventLoop();
    int createEpoll();
    int createListenSocket(int port);
    void removeFromEpoll(int fd);
    void rearmRead(int fd);

    int epollFd_;
    int listenFd_;
    int port_;
    std::unique_ptr<ThreadPool> threadPool_;
    std::vector<std::thread> reactorThreads_;
    std::atomic<bool> running_{false};

    std::function<void(int fd, const std::string& sql)> sqlHandler_;
    std::function<void(int fd)> closeHandler_;

    // 每个连接的应用层接收缓冲区：解决 TCP 粘包/半包
    std::unordered_map<int, std::string> recvBuffers_;
    std::mutex recvBuffersMutex_;
};

}  // namespace minisql
