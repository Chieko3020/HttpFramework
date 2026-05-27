#pragma once

#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <string>
#include "DbConnection.h"

namespace db {

class DbConnectionPool {
public:
    DbConnectionPool(const std::string& host, const std::string& user,
                     const std::string& password, const std::string& database,
                     int port = 3306, int maxConnections = 10);
    ~DbConnectionPool();

    // 禁用拷贝构造和赋值
    DbConnectionPool(const DbConnectionPool&) = delete;
    DbConnectionPool& operator=(const DbConnectionPool&) = delete;

    // 获取连接
    std::shared_ptr<DbConnection> getConnection();

    // 归还连接
    void returnConnection(std::shared_ptr<DbConnection> conn);

    // 初始化连接池
    bool initialize();

    // 关闭连接池
    void shutdown();

    // 获取连接池状态
    size_t getAvailableConnections() const;
    size_t getTotalConnections() const;

    // 健康检查
    void healthCheck();

private:
    std::string host_;
    std::string user_;
    std::string password_;
    std::string database_;
    int port_;
    int maxConnections_;
    
    std::queue<std::shared_ptr<DbConnection>> availableConnections_;
    std::vector<std::shared_ptr<DbConnection>> allConnections_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<bool> shutdown_;
    
    std::thread healthCheckThread_;
    std::atomic<bool> healthCheckRunning_;

    // 创建新连接
    std::shared_ptr<DbConnection> createConnection();
    
    // 健康检查线程函数
    void healthCheckLoop();
};

} // namespace db