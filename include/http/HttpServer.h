#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <map>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <set>
#include <vector>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpContext.h"
#include "TaskTypes.h"
#include "utils/SocketCompat.h"
#include "utils/ThreadPool.h"

namespace router {
class Router;
}

namespace http {

class HttpServer {
public:
    explicit HttpServer(int port,
                        size_t threadPoolSize = std::thread::hardware_concurrency(),
                        size_t subReactorCount = 0);
    explicit HttpServer(int port, utils::ThreadPool& threadPool,
                        size_t subReactorCount = 0);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    // stop() 等待在途业务任务的超时（毫秒，默认 5000）。
    // 共享线程池下，在途任务捕获了 this；stop() 必须等它们归零再释放成员（H3）。
    // 超时只影响"等待多久"，超时后会打印错误但不阻塞关闭流程。
    void setShutdownDrainTimeoutMs(int ms) { drainTimeoutMs_.store(ms > 0 ? ms : 1); }
    int shutdownDrainTimeoutMs() const { return drainTimeoutMs_.load(); }
    // 当前在途业务任务数（含已入队未开始）；仅用于观测与测试
    uint64_t inFlightTaskCount() const { return inFlightTasks_.load(); }

    void setRouter(std::shared_ptr<router::Router> router);

    // 启用内存池。blockSizeBytes / poolBlocks 为 0 时用默认值（12KB / 5000 块）。
    // 必须在 start() 之前调用：池是进程级单例，运行期改参数会让新旧连接
    // 使用不同块容量（H7/L12）。
    void enableMemoryPool(bool enable = true, size_t blockSizeBytes = 0,
                          size_t poolBlocks = 0);
    bool isMemoryPoolEnabled() const { return useMemoryPool_; }
    // 内存池单块容量（即内存池模式下单连接请求的上限）；0 = 未启用/默认
    size_t memoryPoolBlockSize() const { return memoryPoolBlockSize_; }

    // 空闲连接超时（秒）；<=0 表示不启用超时清理
    void setIdleTimeout(int seconds) { idleTimeoutSec_ = seconds; }
    int idleTimeout() const { return idleTimeoutSec_; }

    // 最大并发连接数（默认 10000，0 = 不限制）：超过则拒绝新连接（L7）
    void setMaxConnections(size_t n) { maxConnections_ = n; }
    size_t maxConnections() const { return maxConnections_; }

    // 空闲检查周期（秒，默认 5）：必须在 start() 之前设置。
    // 空闲超时的实际精度 = 该周期（L6）
    void setIdleCheckIntervalSec(int seconds) {
        idleCheckIntervalSec_ = seconds > 0 ? seconds : 5;
    }
    int idleCheckIntervalSec() const { return idleCheckIntervalSec_; }

    // 单请求体上限（字节，默认 64MB）：超过时回 413 而不是让服务端无限等待（M3）
    void setMaxRequestBodyBytes(size_t bytes) {
        maxRequestBodyBytes_ = bytes > 0 ? bytes : 1;
    }
    size_t maxRequestBodyBytes() const { return maxRequestBodyBytes_; }

    // 非池模式下单连接请求缓冲总量上限（默认 1MB）与请求头上限（默认 16KB）（M4）
    void setMaxRequestBytes(size_t bytes) { maxRequestBytes_ = bytes > 0 ? bytes : 1; }
    size_t maxRequestBytes() const { return maxRequestBytes_; }
    void setMaxRequestHeaderBytes(size_t bytes) {
        maxRequestHeaderBytes_ = bytes > 0 ? bytes : 1;
    }
    size_t maxRequestHeaderBytes() const { return maxRequestHeaderBytes_; }

    static bool isPortInUse(int port);

    struct Statistics {
        std::atomic<uint64_t> totalRequests{0};
        std::atomic<uint64_t> activeConnections{0};
        std::atomic<uint64_t> completedRequests{0};
        std::atomic<uint64_t> queuedTasks{0};
    };

    // 统计读取口（L14）：
    //   - 每个字段都是 atomic，读取不会与请求线程竞争，但**不保证跨字段一致**：
    //     多个计数器是分别 load 的，读数之间可能有在途请求被计入，属正常的快照漂移；
    //   - 需要一致口径的用例应取差值与"请求已完成"的同步点配合使用
    //     （见 tests/test_http_hardening.cpp 的 H1/H2 用例）。
    // App::stats() 直接转发到这里（H15）。
    const Statistics& getStatistics() const { return stats_; }

private:
    // 待发送的响应（H5）：worker 生成字节后只把它投递到这里，
    // 由 I/O 线程取出并写入连接。worker 因此完全不触碰 HttpContext 的字段。
    struct PendingResponse {
        net::ConnId connId{0};
        std::shared_ptr<const std::string> data;
        bool keepAlive{false};
    };

    // ---- SubReactor ----
    struct SubReactor {
        int epollFd = -1;
        int wakeFd = -1;           // eventfd, 跨线程通知 sub reactor 有响应待发送
        int timerFd = -1;          // timerfd, 周期性触发空闲连接清理
        std::thread thread;
        std::map<int, std::unique_ptr<HttpContext>> contexts;
        std::mutex contextsMutex;
        std::mutex wakeMutex;      // 保护 pendingResponses 队列
        // 待发送的响应（含连接代际），见 utils/SocketCompat.h 的 ConnId
        std::vector<PendingResponse> pendingResponses;
    };

    // fd 的进程级归属记录：代际 + 所属 sub reactor。
    // 关闭连接只清 owner_ / context_（保留代际），这样"陈旧 fd"与"从未使用的 fd"
    // 可以区分，避免陈旧回调关闭一个已被新连接占用的 fd 号。
    struct FdBinding {
        uint32_t generation = 0;
        int owner = -1;
        bool context = false;
    };

    int port_;
    std::atomic<bool> running_;

    // 监听 socket
    int listenFd_;
    struct sockaddr_in serverAddr_;

    // 主 Reactor（仅 accept + 分发）
    int mainEpollFd_;
    std::thread mainReactorThread_;

    // 子 Reactor 列表（read / write / 业务调度）
    std::vector<std::unique_ptr<SubReactor>> subReactors_;
    size_t subReactorCount_;
    std::atomic<size_t> nextSubReactor_{0};

    // fd 归属表（主 reactor 与全部 sub reactor 共享；见上方 FdBinding）
    mutable std::mutex connMutex_;
    std::unordered_map<int, FdBinding> conns_;

    // 业务线程池（可为外部共享或自拥有）
    utils::ThreadPool* threadPool_{nullptr};
    std::unique_ptr<utils::ThreadPool> ownedPool_;
    size_t threadPoolSize_{4};  // 保存线程数，用于 stop() 后重启时重建

    // 路由
    std::shared_ptr<router::Router> router_;

    // 性能统计
    mutable Statistics stats_;

    // 内存池配置
    bool useMemoryPool_;
    size_t memoryPoolBlockSize_{0};  // 0 = 默认 12KB
    size_t memoryPoolBlocks_{0};     // 0 = 默认 5000 块
    // 本 server 自持的内存池实例（启用内存池时在 initializeServer 里创建）。
    // 用自持实例而不是进程级单例：块容量/块数因此可以按 server 配置，
    // 也不会出现"另一个 server 先创建了全局池导致我的参数被忽略"。
    std::unique_ptr<utils::HttpMemoryPool> ownedMemoryPool_;

    // 空闲连接超时（秒）
    int idleTimeoutSec_{60};
    // 空闲检查周期（秒）
    int idleCheckIntervalSec_{5};
    // 最大并发连接数（L7）
    size_t maxConnections_{10000};

    // 单请求体上限（M3）
    size_t maxRequestBodyBytes_{64u * 1024u * 1024u};
    // 非池模式单连接请求缓冲上限 / 请求头上限（M4）
    size_t maxRequestBytes_{1u * 1024u * 1024u};
    size_t maxRequestHeaderBytes_{16u * 1024u};

    // stop() 关停排空（H3）：在途任务计数 + 归零通知
    std::atomic<uint64_t> inFlightTasks_{0};
    mutable std::mutex drainMutex_;
    std::condition_variable drainCv_;
    std::atomic<int> drainTimeoutMs_{5000};

    // ---- 初始化 ----
    bool initializeServer();
    bool setupMainEpoll();
    bool setupSubReactor(size_t index);
    bool setNonBlocking(int fd);

    // ---- 关停 ----
    // 等待在途业务任务归零；返回是否在超时前归零
    bool waitForInFlightTasks();
    // 业务任务（含排队中的）结束时调用：递减计数并在归零时唤醒 stop()
    void finishInFlightTask();
    // 释放监听/主 epoll/子 reactor 的 fd（幂等，可重复调用）
    void releaseFds();

    // ---- 主 Reactor ----
    void mainReactorLoop();
    void handleAccept();

    // ---- 子 Reactor ----
    void subReactorLoop(int index);
    void handleRead(int clientFd, int subReactorIndex, net::ConnId connId);
    void handleWrite(int clientFd, int subReactorIndex);
    void handleWake(int subReactorIndex);   // 处理 eventfd 唤醒，批量发送响应
    void handleTimer(int subReactorIndex);  // 处理 timerfd 到期，清理空闲连接
    void dispatchBufferedRequests(int clientFd, int subReactorIndex, net::ConnId connId);
    void closeConnection(int fd, int subReactorIndex);
    void closeConnectionId(net::ConnId connId);
    void rearmRead(int clientFd, int subReactorIndex, net::ConnId connId);

    // 业务处理（线程池中执行）
    void enqueueRequestTask(int subReactorIndex, int clientFd, net::ConnId connId,
                            std::shared_ptr<HttpRequest> request,
                            std::shared_ptr<HttpResponse> response);
    void processHttpRequest(int subReactorIndex, net::ConnId connId,
                            std::shared_ptr<HttpRequest> request,
                            std::shared_ptr<HttpResponse> response);

    // ---- 连接归属与代际校验（C2）----
    // 注册一个新连接，返回 (fd, 新代际) 组成的 ConnId
    net::ConnId registerConnection(int fd, int subReactorIndex);
    // 记录/擦除事件监听类 fd（listenFd_ / wakeFd / timerFd）
    void registerEventFd(int fd, int subReactorIndex);
    void unregisterEventFd(int fd);
    // 该连接标识是否仍归本 reactor 所有（陈旧回调返回 false）
    bool ownsConnection(int fd, int subReactorIndex, net::ConnId connId) const;
    // 把 fd 从本 reactor 的待发送队列中移除（连接关闭 / fd 被新连接复用）
    void dropPendingResponses(int subReactorIndex, int fd);
    // 按 (fd, 代际) 增减该连接的在途请求计数（H4）
    void markInFlight(int subReactorIndex, net::ConnId connId, bool enter);
    // 唤醒 sub reactor 去发送该连接的响应（H5：响应内容随队列一起交付）
    void notifyConnectionReady(int subReactorIndex, net::ConnId connId,
                               std::shared_ptr<const std::string> data, bool keepAlive);
    // 取该 fd 当前连接代际（不存在时返回 0）
    net::ConnId connIdOf(int subReactorIndex, int fd) const;
    // 取该 fd 当前连接的 HttpContext（不存在返回 nullptr）
    HttpContext* lookupContext(int subReactorIndex, int fd);
    // 把 ctx 上当前的响应尽量写出去（部分写时挂 EPOLLOUT）
    void writeCurrentResponse(int clientFd, int subReactorIndex, HttpContext* ctx);

    // ---- I/O 辅助 ----
    // 把 socket 上可读的数据直接读进连接的请求缓冲（不再经一个临时 std::string，M1）。
    // peerClosed 明确区分"对端关闭/致命错误"与"暂无数据"（L4）
    size_t readAllData(int clientFd, HttpContext* ctx, bool* peerClosed = nullptr);
    bool writeAllDataFromOffset(int fd, const std::string& data, size_t offset,
                                size_t* bytesWritten = nullptr);
};

} // namespace http
