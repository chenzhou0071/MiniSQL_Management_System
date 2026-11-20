// src/server/network/Reactor.cpp
#include "Reactor.h"
#include "TcpConnection.h"
#include "ThreadPool.h"
#include "SqlProtocol.h"
#include "../../common/Logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <errno.h>

namespace minisql {

// ============================================================
// Reactor事件驱动模型 - 高并发网络服务器核心
// ============================================================
// Reactor模式原理：
// 1. 使用epoll实现I/O多路复用（单线程监听多个socket）
// 2. 事件驱动：socket就绪时触发回调，避免阻塞等待
// 3. 非阻塞I/O：读写操作立即返回，不会阻塞线程
// 4. 线程池处理：将SQL处理任务提交给线程池，Reactor继续监听
//
// 架构组件：
// - epoll：Linux高效I/O多路复用机制
// - 监听socket：接受新客户端连接
// - Reactor线程：2个事件循环线程，监听epoll事件
// - ThreadPool：4个工作线程，处理SQL请求
// - EPOLLONESHOT：防止同一socket在多个线程中并发处理
//
// 工作流程：
// 1. Reactor线程：epoll_wait等待事件
// 2. 新连接：accept -> 注册到epoll（EPOLLONESHOT）
// 3. 可读事件：读取SQL -> 提交到ThreadPool -> 重新注册epoll
// 4. ThreadPool线程：执行SQL -> 返回结果
//
// 性能优势：
// - 单epoll可监听数千个socket（C10K问题解决方案）
// - 避免每连接一线程的开销
// - 线程池复用线程，减少创建/销毁成本
// - EPOLLONESHOT保证连接安全，避免竞态条件
//
// 注意事项：
// - 仅支持Linux（epoll是Linux特有）
// - EPOLLONESHOT需要在处理完成后重新启用（EPOLL_CTL_MOD）
// - SQL处理时socket被禁用，处理完成才能接收新请求
// ============================================================

Reactor::Reactor(int port, int threadCount)
    : port_(port), threadPool_(std::make_unique<ThreadPool>(threadCount)) {
    listenFd_ = createListenSocket(port);
    epollFd_ = createEpoll();
}

Reactor::~Reactor() {
    stop();
    if (epollFd_ >= 0) close(epollFd_);
    if (listenFd_ >= 0) close(listenFd_);
}

// ============================================================
// 创建监听socket（辅助函数）
// ============================================================
// 步骤：
// 1. 创建TCP socket（AF_INET + SOCK_STREAM）
// 2. 设置SO_REUSEADDR（允许立即重用端口，避免TIME_WAIT阻塞）
// 3. 绑定到指定端口（监听所有网络接口INADDR_ANY）
// 4. 开始监听，backlog=128（等待连接队列最大长度）
//
// 返回值：监听socket的文件描述符，失败返回-1
// ============================================================
int Reactor::createListenSocket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);  // 创建TCP socket
    if (fd < 0) {
        LOG_ERROR("Reactor", "Failed to create socket");
        return -1;
    }

    // 设置SO_REUSEADDR：允许端口重用，避免重启时端口占用
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 绑定socket到端口
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;           // IPv4
    addr.sin_addr.s_addr = INADDR_ANY;   // 监听所有网络接口
    addr.sin_port = htons(port);         // 指定端口

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Reactor", "Failed to bind to port " + std::to_string(port));
        close(fd);
        return -1;
    }

    // 开始监听，backlog=128表示等待连接队列长度
    if (listen(fd, 128) < 0) {
        LOG_ERROR("Reactor", "Failed to listen");
        close(fd);
        return -1;
    }

    // 设置监听socket为非阻塞：多个Reactor线程共享同一个listenFd，
    // 惊群时只有一个accept成功，其余必须立即返回，否则会阻塞事件循环
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    LOG_INFO("Reactor", "Listening on port " + std::to_string(port));

    return fd;  // 返回监听socket
}

// ============================================================
// 创建epoll实例（辅助函数）
// ============================================================
// epoll_create1(0)：创建epoll实例
// 返回值：epoll文件描述符，用于epoll_ctl和epoll_wait
// ============================================================
int Reactor::createEpoll() {
    int fd = epoll_create1(0);  // 创建epoll实例
    if (fd < 0) {
        LOG_ERROR("Reactor", "Failed to create epoll");
        return -1;
    }
    return fd;
}

// ============================================================
// 启动Reactor（开始监听事件）
// ============================================================
// 启动步骤：
// 1. 将监听socket注册到epoll（监听新连接事件）
// 2. 启动2个Reactor线程，执行事件循环
//
// 事件循环线程数量：2个（分担epoll_wait的压力）
// ============================================================
void Reactor::start() {
    running_.store(true);  // 设置运行标志

    // 注册监听socket到epoll（监听新连接）
    struct epoll_event ev;
    ev.events = EPOLLIN;         // 监听可读事件（新连接到来）
    ev.data.fd = listenFd_;      // 监听socket
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, listenFd_, &ev);  // 注册到epoll

    // 启动事件循环线程（2个Reactor线程）
    for (int i = 0; i < 2; ++i) {
        reactorThreads_.emplace_back([this] { eventLoop(); });
    }
}

void Reactor::stop() {
    running_.store(false);
    for (auto& t : reactorThreads_) {
        if (t.joinable()) t.join();
    }
    threadPool_->stop();
}

void Reactor::removeFromEpoll(int fd) {
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);

    // 清理该连接的接收缓冲区
    {
        std::lock_guard<std::mutex> lock(recvBuffersMutex_);
        recvBuffers_.erase(fd);
    }

    // 通知上层清理会话（fd 会被内核复用，不清理会导致会话串线）
    if (closeHandler_) {
        closeHandler_(fd);
    }
}

// 重新挂载 EPOLLONESHOT，等待该连接的下一次读事件
void Reactor::rearmRead(int fd) {
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.fd = fd;
    epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev);
}

// ============================================================
// 事件循环（Reactor核心）
// ============================================================
// 工作流程：
// while running:
//   1. epoll_wait等待事件（最多64个事件，超时1000ms）
//   2. 对每个就绪的事件：
//      a) 如果是监听socket -> 处理新连接
//      b) 如果是客户端socket -> 处理读事件
//
// epoll_wait参数：
// - events：事件数组（最多64个）
// - MAX_EVENTS：最大事件数
// - timeout：1000ms（避免无限阻塞，定期检查running_状态）
//
// 事件类型：
// - EPOLLIN：socket可读（新连接或客户端发送数据）
// - EPOLLERR：socket错误
// - EPOLLHUP：socket挂起（客户端关闭）
// ============================================================
void Reactor::eventLoop() {
    const int MAX_EVENTS = 64;  // 每次最多处理64个事件
    struct epoll_event events[MAX_EVENTS];

    while (running_.load()) {  // 循环直到停止
        // epoll_wait等待事件，超时1000ms
        int n = epoll_wait(epollFd_, events, MAX_EVENTS, 1000);

        for (int i = 0; i < n; ++i) {  // 处理所有就绪事件
            int fd = events[i].data.fd;  // 获取socket文件描述符

            if (fd == listenFd_) {
                // 监听socket就绪 -> 新客户端连接
                handleNewConnection();
            } else if (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
                // 客户端socket就绪 -> 可读/错误/挂起
                handleRead(fd);
            }
        }
    }
}

// ============================================================
// 处理新连接（辅助函数）
// ============================================================
// 步骤：
// 1. accept接受新连接，获取客户端socket
// 2. 设置客户端socket为非阻塞模式
// 3. 注册客户端socket到epoll（EPOLLONESHOT）
//
// EPOLLONESHOT作用：
// - 保证同一socket在某个时刻只能被一个线程处理
// - 防止并发处理同一连接导致数据混乱
// - 处理完成后需要重新启用（EPOLL_CTL_MOD）
// ============================================================
void Reactor::handleNewConnection() {
    struct sockaddr_in clientAddr;  // 客户端地址
    socklen_t clientLen = sizeof(clientAddr);

    // accept接受新连接
    int clientFd = accept(listenFd_, (struct sockaddr*)&clientAddr, &clientLen);
    if (clientFd < 0) {
        return;  // accept失败，忽略
    }

    // 设置客户端socket为非阻塞模式（Reactor要求）
    int flags = fcntl(clientFd, F_GETFL, 0);
    fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);

    // 注册客户端socket到epoll，使用EPOLLONESHOT防止并发处理
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLONESHOT;  // 可读事件 + 单次触发
    ev.data.fd = clientFd;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, clientFd, &ev) < 0) {
        close(clientFd);  // 注册失败，关闭连接
        return;
    }
}

// ============================================================
// 处理读事件（核心函数）
// ============================================================
// 步骤：
// 1. 读取socket数据（SQL请求）
// 2. 提交SQL到线程池处理
// 3. 处理完成后重新启用EPOLLONESHOT
//
// EPOLLONESHOT机制：
// - 触发一次后socket被禁用
// - ThreadPool处理SQL期间，socket不会触发新事件
// - 处理完成后通过EPOLL_CTL_MOD重新启用
// - 保证同一连接的请求串行处理
//
// 并发模型：
// - Reactor线程：只负责监听和分发事件（轻量级）
// - ThreadPool线程：执行SQL和返回结果（重量级）
// - 分离I/O和计算，充分利用多核CPU
// ============================================================
void Reactor::handleRead(int fd) {
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));

    if (n == 0) {
        // 对端正常关闭
        removeFromEpoll(fd);
        return;
    }
    if (n < 0) {
        // EAGAIN/EWOULDBLOCK：本次没有更多数据，等下次事件
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            rearmRead(fd);
        } else {
            removeFromEpoll(fd);
        }
        return;
    }

    // 每个连接一条语句为一行：把新数据追加到该连接的应用层缓冲区，
    // 再按 '\n' 切出所有完整语句（解决粘包），不完整的半包留到下次
    const size_t kMaxPendingBytes = 1024 * 1024;  // 单连接缓冲区上限，防止恶意无换行数据打爆内存
    std::vector<std::string> statements;
    bool overflow = false;

    {
        std::lock_guard<std::mutex> lock(recvBuffersMutex_);
        std::string& pending = recvBuffers_[fd];
        pending.append(buf, static_cast<size_t>(n));

        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);

            // 去掉行尾 '\r' 与首尾空白
            size_t first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos) {
                continue;  // 空行
            }
            size_t last = line.find_last_not_of(" \t\r");
            statements.push_back(line.substr(first, last - first + 1));
        }

        if (pending.size() > kMaxPendingBytes) {
            overflow = true;
            pending.clear();
        }
    }

    if (overflow) {
        LOG_WARN("Reactor", "Receive buffer overflow, closing connection");
        removeFromEpoll(fd);
        return;
    }

    if (!sqlHandler_) {
        removeFromEpoll(fd);
        return;
    }

    if (statements.empty()) {
        // 半包：还没凑齐一条完整语句，重新挂载等待后续数据
        rearmRead(fd);
        return;
    }

    // 提交到线程池：同一连接的语句串行执行，全部处理完再重新挂载 EPOLLONESHOT
    threadPool_->submit([this, fd, statements]() {
        for (const auto& sql : statements) {
            sqlHandler_(fd, sql);
        }
        rearmRead(fd);
    });
}

}  // namespace minisql
