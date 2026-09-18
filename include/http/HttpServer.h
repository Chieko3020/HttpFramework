#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <map>
#include <unordered_map>
#include <mutex>
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

    void setRouter(std::shared_ptr<router::Router> router);

    void enableMemoryPool(bool enable = true);
    bool isMemoryPoolEnabled() const { return useMemoryPool_; }

    // 空闲连接超时（秒）；<=0 表示不启用超时清理
    void setIdleTimeout(int seconds) { idleTimeoutSec_ = seconds; }
    int idleTimeout() const { return idleTimeoutSec_; }

    static bool isPortInUse(int port);

    struct Statistics {
        std::atomic<uint64_t> totalRequests{0};
        std::atomic<uint64_t> activeConnections{0};
        std::atomic<uint64_t> completedRequests{0};
        std::atomic<uint64_t> queuedTasks{0};
    };

    const Statistics& getStatistics() const { return stats_; }

private:
    // ---- SubReactor ----
    struct SubReactor {
        int epollFd = -1;
        int wakeFd = -1;           // eventfd, 跨线程通知 sub reactor 有响应待发送
        int timerFd = -1;          // timerfd, 周期性触发空闲连接清理
        std::thread thread;
        std::map<int, std::unique_ptr<HttpContext>> contexts;
        std::mutex contextsMutex;
        std::mutex wakeMutex;      // 保护 pendingWrites 队列
        // 待发送响应的连接标识（含代际），见 utils/SocketCompat.h 的 ConnId
        std::vector<net::ConnId> pendingWrites;
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

    // 空闲连接超时（秒）
    int idleTimeoutSec_{60};

    // ---- 初始化 ----
    bool initializeServer();
    bool setupMainEpoll();
    bool setupSubReactor(size_t index);
    bool setNonBlocking(int fd);

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
    // 该 fd 上是否已存在一个"别的"连接（用于区分陈旧 fd 与自由 fd）
    bool fdTakenByOther(int fd, int subReactorIndex) const;
    // 把 fd 从本 reactor 的 pendingWrites 中移除（连接关闭 / fd 被新连接复用）
    void dropPendingWrites(int subReactorIndex, int fd);
    // 唤醒 sub reactor 去发送该连接的响应
    void notifyConnectionReady(int subReactorIndex, net::ConnId connId);
    // 取该 fd 当前连接代际（不存在时返回 0）
    net::ConnId connIdOf(int subReactorIndex, int fd) const;

    // ---- I/O 辅助 ----
    std::string readAllData(int fd, size_t* totalRead = nullptr);
    bool writeAllDataFromOffset(int fd, const std::string& data, size_t offset,
                                size_t* bytesWritten = nullptr);
};

} // namespace http
