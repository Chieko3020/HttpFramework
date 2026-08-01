#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <map>
#include <mutex>
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
        std::thread thread;
        std::map<int, std::unique_ptr<HttpContext>> contexts;
        std::mutex contextsMutex;
        std::mutex wakeMutex;      // 保护 pendingWrites 队列
        std::vector<int> pendingWrites;  // 待发送响应的 fd 列表
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

    // fd → subReactor 索引（主线程写，其他线程读，写在后读在前已由 epoll 事件保证 happens-before）
    std::unordered_map<int, int> fdToReactor_;
    std::mutex fdToReactorMutex_;

    // 业务线程池（可为外部共享或自拥有）
    utils::ThreadPool* threadPool_{nullptr};
    bool ownsThreadPool_{false};
    size_t threadPoolSize_{4};  // 保存线程数，用于 stop() 后重启时重建

    // 路由
    std::shared_ptr<router::Router> router_;

    // 性能统计
    mutable Statistics stats_;

    // 内存池配置
    bool useMemoryPool_;

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
    void handleRead(int clientFd, int subReactorIndex);
    void handleWrite(int clientFd, int subReactorIndex);
    void handleWake(int subReactorIndex);   // 处理 eventfd 唤醒，批量发送响应
    void closeConnection(int fd, int subReactorIndex);

    // ---- 业务处理（线程池中执行） ----
    void processHttpRequest(int subReactorIndex, int clientFd,
                            std::shared_ptr<HttpRequest> request,
                            std::shared_ptr<HttpResponse> response);

    // ---- I/O 辅助 ----
    std::string readAllData(int fd);
    bool writeAllData(int fd, const std::string& data);
    bool writeAllDataFromOffset(int fd, const std::string& data, size_t offset,
                                size_t* bytesWritten = nullptr);
};

} // namespace http
