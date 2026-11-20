// src/server/network/TcpServer.cpp
#include "TcpServer.h"
#include "Reactor.h"
#include "SqlProtocol.h"
#include "../transaction/TransactionManager.h"
#include "../../executor/Executor.h"
#include "../../parser/Parser.h"
#include "../../common/Logger.h"
#include "../../common/Error.h"
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <cstring>
#include <errno.h>

namespace minisql {

// 全局服务器指针,用于信号处理
static TcpServer* g_server = nullptr;

void signalHandler(int sig) {
    if (g_server) {
        LOG_INFO("TcpServer", "Received signal " + std::to_string(sig) + ", shutting down...");
        g_server->stop();
    }
}

TcpServer::TcpServer(int port, int threadCount)
    : port_(port), threadCount_(threadCount) {
    reactor_ = std::make_unique<Reactor>(port, threadCount);

    // 连接关闭时清理会话（fd 会被内核复用，不清理会导致会话串线）
    reactor_->setCloseHandler([this](int fd) {
        removeSession(fd);
    });

    // 设置SQL处理函数
    reactor_->setSqlHandler([this](int fd, const std::string& sql) {
        LOG_INFO("TcpServer", "Received SQL: [" + sql + "]");

        // 取当前连接的会话状态（内部加锁，返回值拷贝）
        std::string currentDb = getSessionDatabase(fd);

        // 事务上下文属于连接而非线程：先把该连接的事务恢复到当前工作线程
        TransactionManager::setCurrentTransaction(getSessionTransaction(fd));

        SqlResponse response;

        try {
            // 解析 SQL
            parser::Lexer lexer(sql);
            parser::Parser parser(lexer);
            auto stmt = parser.parseStatement();

            // 执行 SQL（使用会话的数据库）
            auto& executor = executor::Executor::getInstance();
            auto execResult = executor.execute(stmt.get(), currentDb);

            // 如果是 USE 语句，更新会话的数据库
            if (execResult.isSuccess() && stmt->getType() == parser::ASTNodeType::USE_STMT) {
                auto useStmt = dynamic_cast<parser::UseStmt*>(stmt.get());
                if (useStmt) {
                    setSessionDatabase(fd, useStmt->database);
                    currentDb = useStmt->database;
                    LOG_INFO("TcpServer", "Session database updated to: " + useStmt->database);
                }
            }

            if (!execResult.isSuccess()) {
                response.success = false;
                response.message = execResult.getError().what();
            } else {
                response.success = true;
                response.message = "Query OK";

                // 获取列名和数据
                auto* result = execResult.getValue();
                if (result) {
                    response.rowCount = result->getRowCount();
                    response.columns = result->columnNames;

                    // 转换 Tuple -> vector<string>
                    for (auto& row : result->rows) {
                        std::vector<std::string> rowStr;
                        for (auto& val : row) {
                            rowStr.push_back(val.toString());
                        }
                        response.rows.push_back(std::move(rowStr));
                    }
                }
            }

        } catch (const MiniSQLException& e) {
            LOG_ERROR("TcpServer", "MiniSQLException caught: " + std::string(e.what()));
            response.success = false;
            response.message = e.what();
        } catch (const std::exception& e) {
            LOG_ERROR("TcpServer", "std::exception caught: " + std::string(e.what()));
            response.success = false;
            response.message = std::string("Internal error: ") + e.what();
        } catch (...) {
            LOG_ERROR("TcpServer", "Unknown exception caught");
            response.success = false;
            response.message = "Unknown error occurred";
        }

        // 保存事务上下文：BEGIN 后会变成新事务指针，
        // COMMIT/ROLLBACK 后变成 nullptr，下次请求进来再恢复
        setSessionTransaction(fd, TransactionManager::getCurrentTransaction());

        std::string resp = SqlProtocol::buildResponse(response);

        // 直接发送响应
        ssize_t sent = ::send(fd, resp.c_str(), resp.size(), 0);
        if (sent < 0) {
            LOG_ERROR("TcpServer", "send() failed: " + std::string(strerror(errno)));
        }
    });
}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::start() {
    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    g_server = this;

    LOG_INFO("TcpServer", "Starting server on port " + std::to_string(port_));

    reactor_->start();

    // 主线程等待
    while (true) {
        sleep(1);
    }
}

void TcpServer::stop() {
    reactor_->stop();

    // 清理所有会话
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_.clear();
    }

    LOG_INFO("TcpServer", "Server stopped");
}

std::string TcpServer::getSessionDatabase(int fd) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    return sessions_[fd].getCurrentDatabase();
}

void TcpServer::setSessionDatabase(int fd, const std::string& db) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessions_[fd].setCurrentDatabase(db);
}

Transaction* TcpServer::getSessionTransaction(int fd) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    return sessions_[fd].getTransaction();
}

void TcpServer::setSessionTransaction(int fd, Transaction* txn) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessions_[fd].setTransaction(txn);
}

void TcpServer::removeSession(int fd) {
    Transaction* txn = nullptr;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = sessions_.find(fd);
        if (it == sessions_.end()) {
            return;
        }
        txn = it->second.getTransaction();
        sessions_.erase(it);
    }

    // 连接断开时还有未提交事务，必须回滚，避免留下部分写入
    if (txn) {
        LOG_INFO("TcpServer", "Connection closed with active transaction, rolling back");
        TransactionManager::setCurrentTransaction(txn);
        TransactionManager::getInstance().rollback();
        TransactionManager::setCurrentTransaction(nullptr);
    }
}

}  // namespace minisql
