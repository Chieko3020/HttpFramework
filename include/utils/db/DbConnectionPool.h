#pragma once

#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
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

    // 获取连接（无限等待；池关闭时返回 nullptr）
    std::shared_ptr<DbConnection> getConnection();

    // 带超时的获取连接：池耗尽时在 timeout 后返回 nullptr，而不是把请求线程永久挂住（M13）
    std::shared_ptr<DbConnection> getConnection(std::chrono::milliseconds timeout);

    // 归还连接（只回收本池已知的连接，避免"归还别的池/已析构连接"破坏池状态）
    void returnConnection(std::shared_ptr<DbConnection> conn);

    // 初始化连接池（幂等：重复调用直接返回已有状态，不再对 joinable 线程赋值
    // —— 那会直接 std::terminate）（M13）
    bool initialize();

    // 关闭连接池（幂等）
    void shutdown();

    // ── RAII 借还（M13）──
    // 作用域结束时自动归还，异常路径也不会漏还。
    class ConnectionGuard {
    public:
        ConnectionGuard() = default;
        ConnectionGuard(DbConnectionPool* pool, std::shared_ptr<DbConnection> conn)
            : pool_(pool), conn_(std::move(conn)) {}
        ~ConnectionGuard() { reset(); }
        ConnectionGuard(const ConnectionGuard&) = delete;
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;
        ConnectionGuard(ConnectionGuard&& other) noexcept
            : pool_(other.pool_), conn_(std::move(other.conn_)) {
            other.pool_ = nullptr;
        }

        // 借出一个连接（带超时）；失败时 get() 返回 nullptr
        static ConnectionGuard acquire(DbConnectionPool& pool,
                                       std::chrono::milliseconds timeout);

        void reset() {
            if (pool_ && conn_) pool_->returnConnection(conn_);
            conn_.reset();
            pool_ = nullptr;
        }
        DbConnection* get() const { return conn_.get(); }
        explicit operator bool() const { return conn_ != nullptr; }
        std::shared_ptr<DbConnection> shared() const { return conn_; }

    private:
        DbConnectionPool* pool_{nullptr};
        std::shared_ptr<DbConnection> conn_;
    };

    // 获取连接池状态
    size_t getAvailableConnections() const;
    size_t getTotalConnections() const;

    // 最近一次 initialize() 失败的原因（成功时为空）。让调用方/测试能区分
    // "服务端连不上"与"服务端可达但凭据/库被拒" —— 两者都返回 false，
    // 但含义完全不同（测试用例此前无法区分，属"因错误的原因通过"）。
    const std::string& lastInitError() const { return lastInitError_; }
    // 服务端是否可达（TCP 端口能否连上；走 getaddrinfo，支持主机名与 IPv6）。
    // initialize() 会更新该值。
    bool serverReachable() const { return serverReachable_; }
    // 主机名/地址能否解析（getaddrinfo 成功）。解析失败时 lastInitError() 报的是
    // "无法解析/探测"，而不是误导性的 "TCP connect failed"。
    bool serverResolvable() const { return serverResolvable_; }

    // 无副作用的 TCP 探测结果（不建 MySQL 会话、不改连接池状态）：
    // available / refused（连得上但被拒或超时）/ unresolved（主机名解析失败）。
    enum class ProbeResult { Available, Refused, Unresolved };
    static ProbeResult probeServer(const std::string& host, int port);

    // 健康检查（只检查空闲连接：借出中的连接归业务线程所有，
    // MySQL Connector/C++ 的 Connection 不是线程安全的）（M13）
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
    std::atomic<bool> initialized_{false};
    std::string lastInitError_;
    bool serverReachable_{false};
    bool serverResolvable_{true};

    // 创建新连接
    std::shared_ptr<DbConnection> createConnection();
    
    // 健康检查线程函数
    void healthCheckLoop();
};

} // namespace db