#include "utils/db/DbConnectionPool.h"
#include <iostream>
#include <chrono>

namespace db {

DbConnectionPool::DbConnectionPool(const std::string& host, const std::string& user,
                                   const std::string& password, const std::string& database,
                                   int port, int maxConnections)
    : host_(host), user_(user), password_(password), database_(database),
      port_(port), maxConnections_(maxConnections), shutdown_(false), 
      healthCheckRunning_(false) {
}

DbConnectionPool::~DbConnectionPool() {
    shutdown();
}

bool DbConnectionPool::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        // 创建初始连接
        for (int i = 0; i < maxConnections_; ++i) {
            auto conn = createConnection();
            if (conn && conn->connect()) {
                availableConnections_.push(conn);
                allConnections_.push_back(conn);
            } else {
                std::cerr << "[ERROR][数据库]：创建连接失败, index=" << i << std::endl;
            }
        }
        
        if (availableConnections_.empty()) {
            std::cerr << "[ERROR][数据库]：所有数据库连接创建失败" << std::endl;
            return false;
        }
        
        // 启动健康检查线程
        healthCheckRunning_ = true;
        healthCheckThread_ = std::thread(&DbConnectionPool::healthCheckLoop, this);
        
        std::cout << "[INFO][数据库]：连接池初始化完成, 连接数=" 
                  << availableConnections_.size() << "" << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "[ERROR][数据库]：连接池初始化失败: " << e.what() << std::endl;
        return false;
    }
}

void DbConnectionPool::shutdown() {
    shutdown_ = true;
    
    // 停止健康检查线程
    if (healthCheckThread_.joinable()) {
        healthCheckRunning_ = false;
        condition_.notify_all();
        healthCheckThread_.join();
    }
    
    // 关闭所有连接（使用超时机制）
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 快速关闭可用连接
    while (!availableConnections_.empty()) {
        auto conn = availableConnections_.front();
        availableConnections_.pop();
        try {
            conn->disconnect();
        } catch (const std::exception& e) {
            // 忽略关闭错误，继续关闭其他连接
        }
    }
    
    // 关闭所有连接
    for (auto& conn : allConnections_) {
        try {
            conn->disconnect();
        } catch (const std::exception& e) {
            // 忽略关闭错误，继续关闭其他连接
        }
    }
    
    allConnections_.clear();
    std::cout << "[INFO][数据库]：连接池已关闭" << std::endl;
}

std::shared_ptr<DbConnection> DbConnectionPool::getConnection() {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // 等待可用连接
    condition_.wait(lock, [this] { 
        return !availableConnections_.empty() || shutdown_; 
    });
    
    if (shutdown_) {
        return nullptr;
    }
    
    auto conn = availableConnections_.front();
    availableConnections_.pop();
    
    return conn;
}

void DbConnectionPool::returnConnection(std::shared_ptr<DbConnection> conn) {
    if (!conn) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (shutdown_) {
        conn->disconnect();
        return;
    }
    
    // 检查连接是否仍然有效
    if (conn->isConnected()) {
        availableConnections_.push(conn);
        condition_.notify_one();
    } else {
        // 连接无效，创建新连接替换
        auto newConn = createConnection();
        if (newConn && newConn->connect()) {
            availableConnections_.push(newConn);
            // 替换allConnections_中的连接
            for (auto& existingConn : allConnections_) {
                if (existingConn == conn) {
                    existingConn = newConn;
                    break;
                }
            }
            condition_.notify_one();
        }
    }
}

size_t DbConnectionPool::getAvailableConnections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return availableConnections_.size();
}

size_t DbConnectionPool::getTotalConnections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allConnections_.size();
}

void DbConnectionPool::healthCheck() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& conn : allConnections_) {
        if (!conn->isConnected()) {
            std::cout << "[INFO][数据库]：检测到断连，尝试重连..." << std::endl;
            if (conn->connect()) {
                std::cout << "[DEBUG][数据库]：重连成功" << std::endl;
            } else {
                std::cerr << "[ERROR][数据库]：重连失败" << std::endl;
            }
        }
    }
}

std::shared_ptr<DbConnection> DbConnectionPool::createConnection() {
    return std::make_shared<DbConnection>(host_, user_, password_, database_, port_);
}

void DbConnectionPool::healthCheckLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (healthCheckRunning_) {
        if (condition_.wait_for(lock, std::chrono::seconds(30),
            [this] { return !healthCheckRunning_; })) {
            break;
        }
        lock.unlock();
        if (healthCheckRunning_) {
            healthCheck();
        }
        lock.lock();
    }
}

} // namespace db
