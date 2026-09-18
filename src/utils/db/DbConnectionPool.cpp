#include "utils/db/DbConnectionPool.h"
#include <iostream>
#include <chrono>
#include <cstring>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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

namespace {

// ── TCP 可连性探测 ─────────────────────────────────────────────────
// 结果三态：Reachable / Refused（连得上但被拒或超时）/ Unresolved（主机名或
// 地址解析不了）。用它区分"服务端不可达"与"服务端可达但拒绝凭据/库"。
//
// 解析必须走 getaddrinfo：原来只认 IPv4 字面量（inet_pton(AF_INET,...)），
// 于是 host="localhost" 或 "::1" 一律解析失败 ⇒ serverReachable=false ⇒
// lastInitError 误报 "MySQL server unreachable (TCP connect failed)"，
// 而 Connector/C++ 自己其实连得上（日志里就出现过 connecting to '::1:13306'）。
using ProbeResult = DbConnectionPool::ProbeResult;

ProbeResult probeTcp(const std::string& host, int port) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;        // 同时接受 IPv4 / IPv6
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
        return ProbeResult::Unresolved;

    ProbeResult result = ProbeResult::Refused;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv;
        tv.tv_sec = 1;                  // 1 秒超时（SO_SNDTIMEO 对 connect 有效）
        tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int rc = ::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
        ::close(fd);
        if (rc == 0) { result = ProbeResult::Available; break; }
    }
    ::freeaddrinfo(res);
    return result;
}

}  // namespace

DbConnectionPool::ProbeResult DbConnectionPool::probeServer(const std::string& host, int port) {
    return probeTcp(host, port);
}

bool DbConnectionPool::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_.load()) {
        return true;   // 幂等（M13）
    }

    const ProbeResult probe = probeTcp(host_, port_);
    serverReachable_ = (probe == ProbeResult::Available);
    serverResolvable_ = (probe != ProbeResult::Unresolved);
    lastInitError_.clear();

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
            // 三态文案：不可达 / 主机解析不了 / 可达但被拒。
            // 不要把"解析失败"说成 "TCP connect failed" —— 那会把 host="localhost"
            // 或 IPv6 误诊成网络不通（Connector 自己能解析，日志里会出现真实地址）。
            if (!serverResolvable_) {
                lastInitError_ = "MySQL host cannot be resolved or probed: " + host_;
            } else if (serverReachable_) {
                lastInitError_ = "server reachable but MySQL rejected connection (credentials/schema)";
            } else {
                lastInitError_ = "MySQL server unreachable (TCP connect failed)";
            }
            std::cerr << "[ERROR][数据库]：所有数据库连接创建失败 (" << lastInitError_ << ")"
                      << std::endl;
            return false;
        }
        
        // 启动健康检查线程（先置 initialized_，避免重复 initialize 二次赋值 joinable 线程）
        if (healthCheckThread_.joinable()) {
            return true;
        }
        healthCheckRunning_ = true;
        healthCheckThread_ = std::thread(&DbConnectionPool::healthCheckLoop, this);
        initialized_.store(true);
        
        std::cout << "[INFO][数据库]：连接池初始化完成, 连接数=" 
                  << availableConnections_.size() << "" << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        lastInitError_ = std::string("exception: ") + e.what();
        std::cerr << "[ERROR][数据库]：连接池初始化失败: " << e.what() << std::endl;
        return false;
    }
}

void DbConnectionPool::shutdown() {
    if (shutdown_.exchange(true)) {
        return;   // 幂等（M13）
    }
    
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
    initialized_.store(false);
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

std::shared_ptr<DbConnection> DbConnectionPool::getConnection(
    std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);

    // 带超时：池耗尽时不再让请求线程永久阻塞（M13）
    bool ready = condition_.wait_for(lock, timeout, [this] {
        return !availableConnections_.empty() || shutdown_;
    });

    if (!ready || shutdown_ || availableConnections_.empty()) {
        return nullptr;
    }

    auto conn = availableConnections_.front();
    availableConnections_.pop();
    return conn;
}

DbConnectionPool::ConnectionGuard DbConnectionPool::ConnectionGuard::acquire(
    DbConnectionPool& pool, std::chrono::milliseconds timeout) {
    return ConnectionGuard(&pool, pool.getConnection(timeout));
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
    
    // 只回收本池已知的连接（M13）：否则会把别人/已析构的连接塞进池里
    bool known = false;
    for (const auto& existing : allConnections_) {
        if (existing == conn) { known = true; break; }
    }
    if (!known) {
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

    // 只检查空闲连接（M13）：借出中的连接正在被业务线程使用，
    // MySQL Connector/C++ 的 Connection 不是线程安全的，并发使用会协议错乱。
    std::queue<std::shared_ptr<DbConnection>> tmp;
    while (!availableConnections_.empty()) {
        auto conn = availableConnections_.front();
        availableConnections_.pop();
        if (!conn) continue;
        if (!conn->isConnected()) {
            std::cout << "[INFO][数据库]：检测到空闲连接断连，尝试重连..." << std::endl;
            if (conn->connect()) {
                std::cout << "[DEBUG][数据库]：重连成功" << std::endl;
            } else {
                std::cerr << "[ERROR][数据库]：重连失败" << std::endl;
            }
        }
        tmp.push(conn);
    }
    availableConnections_.swap(tmp);
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
