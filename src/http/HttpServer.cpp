#include "http/HttpServer.h"
#include "router/Router.h"
#include "router/RouterHandler.h"

#include <algorithm>
#include <iostream>
#include <cstring>
#include <errno.h>
#include <chrono>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

namespace http {

namespace {
// 子 Reactor 的事件掩码：边沿触发 + oneshot（每次事件后需显式重新武装）
constexpr uint32_t kConnReadEvents = EPOLLIN | EPOLLET | EPOLLONESHOT;
constexpr uint32_t kConnWriteEvents = EPOLLOUT | EPOLLET | EPOLLONESHOT;
}  // namespace

HttpServer::HttpServer(int port, size_t threadPoolSize, size_t subReactorCount)
    : port_(port), running_(false), listenFd_(-1), mainEpollFd_(-1),
      threadPoolSize_(threadPoolSize), useMemoryPool_(false) {

    // 进程早期兜底：忽略 SIGPIPE，所有 socket 写入走 MSG_NOSIGNAL。
    // 对端 RST 后 write() 会投递 SIGPIPE，默认动作是终止进程。
    net::ensureSigpipeIgnored();

    if (subReactorCount == 0)
        subReactorCount = std::thread::hardware_concurrency();
    if (subReactorCount < 1)
        subReactorCount = 1;
    subReactorCount_ = subReactorCount;

    ownedPool_ = std::make_unique<utils::ThreadPool>(threadPoolSize);
    threadPool_ = ownedPool_.get();

    std::cout << "[INFO][HTTP服务器]：初始化完成, port=" << port
              << " 配置:" << subReactorCount_ << " 子Reactor"
              << " and " << threadPoolSize << " 工作线程" << std::endl;
}

HttpServer::HttpServer(int port, utils::ThreadPool& threadPool, size_t subReactorCount)
    : port_(port), running_(false), listenFd_(-1), mainEpollFd_(-1), useMemoryPool_(false) {

    net::ensureSigpipeIgnored();

    if (subReactorCount == 0)
        subReactorCount = std::thread::hardware_concurrency();
    if (subReactorCount < 1)
        subReactorCount = 1;
    subReactorCount_ = subReactorCount;

    threadPool_ = &threadPool;
    threadPoolSize_ = threadPool.getThreadCount();

    std::cout << "[INFO][HTTP服务器]：初始化完成, port=" << port
              << " 配置:" << subReactorCount_ << " 子Reactor"
              << " (共享线程池)" << std::endl;
}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    if (running_.load()) {
        std::cerr << "[WARN][HTTP服务器]：服务已在运行" << std::endl;
        return false;
    }

    // 如果线程池已被 shutdown（例如 stop() 后重启），重新创建（仅自拥有池）
    if (threadPool_ && !threadPool_->isRunning() && ownedPool_) {
        ownedPool_.reset(new utils::ThreadPool(threadPoolSize_));
        threadPool_ = ownedPool_.get();
    }


    if (!initializeServer()) {
        return false;
    }

    running_.store(true);

    // 启动所有子 Reactor 线程
    for (size_t i = 0; i < subReactorCount_; ++i) {
        subReactors_[i]->thread = std::thread([this, i]() {
            subReactorLoop(static_cast<int>(i));
        });
    }

    // 启动主 Reactor 线程（accept）
    mainReactorThread_ = std::thread([this]() {
        mainReactorLoop();
    });

    std::cout << "[INFO][HTTP服务器]：服务启动, port=" << port_
              << " (主线程+" << subReactorCount_ << " 子Reactor)" << std::endl;
    return true;
}

void HttpServer::stop() {
    // exchange 而非 load+store：并发调用 stop() 时只有一个执行关停流程；
    // 同时 stop() 不再因 running_ 为 false 提前返回 —— start() 失败的路径
    // 已经分配过 fd，必须由这里兜底释放（M5 的前提）。
    const bool wasRunning = running_.exchange(false);

    if (wasRunning) {
        // 唤醒子 Reactor：这里**不关闭** fd，只写 eventfd。
        // 在途任务仍可能向 wakeFd 写入（notifyConnectionReady），
        // 若先 close，那一笔写会落到被复用的 fd 号上。
        for (auto& sr : subReactors_) {
            if (sr && sr->wakeFd >= 0) {
                uint64_t one = 1;
                ssize_t n = ::write(sr->wakeFd, &one, sizeof(one));
                (void)n;
            }
        }

        // 等待 I/O 线程退出（两个 Reactor 的 epoll_wait 超时均为 1s，
        // 最坏 1s 内自然退出，无需依赖关闭 fd 来打断它们）
        if (mainReactorThread_.joinable()) {
            mainReactorThread_.join();
        }
        for (auto& sr : subReactors_) {
            if (sr && sr->thread.joinable()) {
                sr->thread.join();
            }
        }

        // I/O 线程已全部退出 ⇒ 不会再有新的任务入队。
        // 在释放任何成员之前，必须等在途（含排队中）的业务任务归零：
        // 任务体是 this 的成员函数（捕获 this），共享线程池的生命周期由调用方
        // 掌控，析构后 worker 仍在访问 this 就是 UAF（H3）。
        waitForInFlightTasks();
    }

    // 释放 fd（幂等）
    releaseFds();

    // 关闭线程池（仅当自拥有时，共享池由 App 管理生命周期）
    if (ownedPool_) {
        threadPool_->shutdown();
        ownedPool_.reset();
        threadPool_ = nullptr;
    }

    // 清理所有子 Reactor 的连接
    for (auto& sr : subReactors_) {
        if (!sr) continue;
        std::lock_guard<std::mutex> lock(sr->contextsMutex);
        for (auto& pair : sr->contexts) {
            close(pair.first);
        }
        sr->contexts.clear();
    }

    // 清理连接归属表
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        conns_.clear();
    }

    // 连接表已清空（所有 PooledBuffer 均已归还），此时才能销毁自持内存池
    ownedMemoryPool_.reset();

    if (wasRunning) {
        std::cout << "[INFO][HTTP服务器]：服务已停止" << std::endl;
    }
}

bool HttpServer::waitForInFlightTasks() {
    if (inFlightTasks_.load(std::memory_order_acquire) == 0) return true;

    std::unique_lock<std::mutex> lock(drainMutex_);
    const int timeoutMs = drainTimeoutMs_.load();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    // 谓词在持锁状态下求值、通知方也持锁，因此不存在"丢失唤醒"
    bool drained = drainCv_.wait_until(lock, deadline, [this] {
        return inFlightTasks_.load(std::memory_order_acquire) == 0;
    });
    if (!drained) {
        std::cerr << "[ERROR][HTTP服务器]：关闭等待超时, 仍有 "
                  << inFlightTasks_.load() << " 个在途任务未结束（>"
                  << timeoutMs << "ms）；请先排空线程池再销毁 HttpServer"
                  << std::endl;
    }
    return drained;
}

void HttpServer::finishInFlightTask() {
    if (inFlightTasks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // 持锁通知：与 waitForInFlightTasks 的谓词求值互斥，避免丢失唤醒
        std::lock_guard<std::mutex> lock(drainMutex_);
        drainCv_.notify_all();
    }
}

void HttpServer::releaseFds() {
    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }

    if (mainEpollFd_ >= 0) {
        close(mainEpollFd_);
        mainEpollFd_ = -1;
    }

    for (auto& sr : subReactors_) {
        if (!sr) continue;
        if (sr->epollFd >= 0) {
            close(sr->epollFd);
            sr->epollFd = -1;
        }
        if (sr->wakeFd >= 0) {
            close(sr->wakeFd);
            sr->wakeFd = -1;
        }
        if (sr->timerFd >= 0) {
            close(sr->timerFd);
            sr->timerFd = -1;
        }
    }
}

bool HttpServer::initializeServer() {
    // 创建监听 socket
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::cerr << "[ERROR][HTTP服务器]：创建socket失败: " << strerror(errno) << std::endl;
        return false;
    }

    int opt = 1;
    if (setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "[ERROR][HTTP服务器]：设置SO_REUSEADDR失败: " << strerror(errno) << std::endl;
        close(listenFd_);
        return false;
    }

    // 注意：不使用 SO_REUSEPORT。
    // SO_REUSEPORT 允许多个 socket 绑定同一端口，内核轮询分发连接。
    // 在多进程负载均衡场景中有用，但本框架各 HttpServer 实例持有独立的 Router
    // 和 ThreadPool，并行绑定会导致路由不一致、资源隔离失效。
    // 仅使用 SO_REUSEADDR（允许 TIME_WAIT 端口快速重用）。

    if (!setNonBlocking(listenFd_)) {
        std::cerr << "[ERROR][HTTP服务器]：设置非阻塞失败: " << strerror(errno) << std::endl;
        close(listenFd_);
        return false;
    }

    memset(&serverAddr_, 0, sizeof(serverAddr_));
    serverAddr_.sin_family = AF_INET;
    serverAddr_.sin_addr.s_addr = INADDR_ANY;
    serverAddr_.sin_port = htons(port_);

    if (bind(listenFd_, (struct sockaddr*)&serverAddr_, sizeof(serverAddr_)) < 0) {
        std::cerr << "[ERROR][HTTP服务器]：绑定端口失败, port=" << port_ << ": " << strerror(errno) << std::endl;
        close(listenFd_);
        return false;
    }

    if (listen(listenFd_, SOMAXCONN) < 0) {
        std::cerr << "[ERROR][HTTP服务器]：监听失败: " << strerror(errno) << std::endl;
        close(listenFd_);
        return false;
    }

    // 内存池：由本 server 自持，块容量/块数在启动时落定（H7/L12）
    if (useMemoryPool_) {
        ownedMemoryPool_ = std::make_unique<utils::HttpMemoryPool>(
            memoryPoolBlocks_ ? memoryPoolBlocks_ : utils::HttpMemoryPool::DEFAULT_POOL_SIZE,
            memoryPoolBlockSize_ ? memoryPoolBlockSize_ : utils::HttpMemoryPool::BLOCK_SIZE);
    }

    // 初始化主 epoll（仅监听 listenFd_）
    if (!setupMainEpoll()) {
        close(listenFd_);
        return false;
    }

    // 初始化子 Reactor
    subReactors_.reserve(subReactorCount_);
    for (size_t i = 0; i < subReactorCount_; ++i) {
        auto sr = std::make_unique<SubReactor>();
        subReactors_.push_back(std::move(sr));
        if (!setupSubReactor(i)) {
            close(listenFd_);
            return false;
        }
    }

    std::cout << "[INFO][HTTP服务器]：初始化完成, port=" << port_
              << " 配置:" << subReactorCount_ << " 子Reactor" << std::endl;
    return true;
}

bool HttpServer::setupMainEpoll() {
    mainEpollFd_ = epoll_create1(EPOLL_CLOEXEC);
    if (mainEpollFd_ < 0) {
        std::cerr << "[ERROR][HTTP服务器]：创建主epoll失败: " << strerror(errno) << std::endl;
        return false;
    }

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = net::makeConnId(listenFd_, 0);

    if (epoll_ctl(mainEpollFd_, EPOLL_CTL_ADD, listenFd_, &event) < 0) {
        std::cerr << "[ERROR][HTTP服务器]：添加监听socket到主epoll失败: " << strerror(errno) << std::endl;
        close(mainEpollFd_);
        mainEpollFd_ = -1;
        return false;
    }

    return true;
}

bool HttpServer::setupSubReactor(size_t index) {
    auto& sr = subReactors_[index];
    sr->epollFd = epoll_create1(EPOLL_CLOEXEC);
    if (sr->epollFd < 0) {
        std::cerr << "[ERROR][HTTP服务器]：创建子Reactor epoll失败, index=" << index
                  << ": " << strerror(errno) << std::endl;
        return false;
    }

    // 创建 eventfd 用于跨线程唤醒（worker 线程通知 sub reactor 有响应待发送）
    sr->wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (sr->wakeFd < 0) {
        std::cerr << "[ERROR][HTTP服务器]：创建 eventfd 失败, index=" << index
                  << ": " << strerror(errno) << std::endl;
        close(sr->epollFd);
        sr->epollFd = -1;
        return false;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.u64 = net::makeConnId(sr->wakeFd, 0);
    if (epoll_ctl(sr->epollFd, EPOLL_CTL_ADD, sr->wakeFd, &ev) < 0) {
        std::cerr << "[ERROR][HTTP服务器]：注册 eventfd 到 epoll 失败, index=" << index
                  << ": " << strerror(errno) << std::endl;
        close(sr->wakeFd);
        close(sr->epollFd);
        sr->wakeFd = -1;
        sr->epollFd = -1;
        return false;
    }

    // 创建 timerfd：周期性触发空闲连接清理（与 eventfd 同模式，逻辑跑在 I/O 线程内）
    sr->timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (sr->timerFd < 0) {
        std::cerr << "[ERROR][HTTP服务器]：创建 timerfd 失败, index=" << index
                  << ": " << strerror(errno) << std::endl;
        close(sr->wakeFd);
        close(sr->epollFd);
        sr->wakeFd = -1;
        sr->epollFd = -1;
        return false;
    }

    struct itimerspec its{};
    its.it_interval.tv_sec = 5;   // 每 5s 检查一次空闲连接
    its.it_value.tv_sec = 5;
    if (timerfd_settime(sr->timerFd, 0, &its, nullptr) < 0) {
        std::cerr << "[ERROR][HTTP服务器]：设置 timerfd 失败, index=" << index
                  << ": " << strerror(errno) << std::endl;
        close(sr->timerFd);
        close(sr->wakeFd);
        close(sr->epollFd);
        sr->timerFd = -1;
        sr->wakeFd = -1;
        sr->epollFd = -1;
        return false;
    }

    ev.events = EPOLLIN;   // 非 ET：每次唤醒都读取到期计数即可
    ev.data.u64 = net::makeConnId(sr->timerFd, 0);
    if (epoll_ctl(sr->epollFd, EPOLL_CTL_ADD, sr->timerFd, &ev) < 0) {
        std::cerr << "[ERROR][HTTP服务器]：注册 timerfd 到 epoll 失败, index=" << index
                  << ": " << strerror(errno) << std::endl;
        close(sr->timerFd);
        close(sr->wakeFd);
        close(sr->epollFd);
        sr->timerFd = -1;
        sr->wakeFd = -1;
        sr->epollFd = -1;
        return false;
    }

    // 登记归属（代际 0）：避免这些 fd 被误判为"可被陈旧回调关闭的连接 fd"
    registerEventFd(sr->epollFd, static_cast<int>(index));
    registerEventFd(sr->wakeFd, static_cast<int>(index));
    registerEventFd(sr->timerFd, static_cast<int>(index));

    return true;
}

bool HttpServer::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

void HttpServer::setRouter(std::shared_ptr<router::Router> router) {
    router_ = router;
}

void HttpServer::enableMemoryPool(bool enable, size_t blockSizeBytes, size_t poolBlocks) {
    if (running_.load()) {
        // 池参数是进程级/连接级的静态配置，运行期变更会造成"新旧连接块大小不一致"，
        // 这类差异极难复现。宁可显式拒绝并提示（H7/L12）。
        std::cerr << "[WARN][HTTP服务器]：服务已在运行，enableMemoryPool 被忽略"
                     "（必须在 start() 之前调用）" << std::endl;
        return;
    }
    useMemoryPool_ = enable;
    if (blockSizeBytes > 0) {
        memoryPoolBlockSize_ =
            std::min(std::max(blockSizeBytes, utils::HttpMemoryPool::MIN_BLOCK_SIZE),
                     utils::HttpMemoryPool::BLOCK_SIZE);
    }
    if (poolBlocks > 0) memoryPoolBlocks_ = poolBlocks;
    std::cout << "[INFO][HTTP服务器]：内存池已" << (enable ? "启用" : "禁用")
              << " (块容量=" << (memoryPoolBlockSize_ ? memoryPoolBlockSize_
                                                      : utils::HttpMemoryPool::BLOCK_SIZE)
              << " 字节, 块数=" << (memoryPoolBlocks_ ? memoryPoolBlocks_
                                                     : utils::HttpMemoryPool::DEFAULT_POOL_SIZE)
              << ")" << std::endl;
}

// ---- 连接归属与代际校验 ----

net::ConnId HttpServer::registerConnection(int fd, int subReactorIndex) {
    uint32_t gen = net::nextConnectionGeneration();
    std::lock_guard<std::mutex> lock(connMutex_);
    auto& binding = conns_[fd];
    binding.generation = gen;
    binding.owner = subReactorIndex;
    binding.context = true;
    return net::makeConnId(fd, gen);
}

void HttpServer::registerEventFd(int fd, int subReactorIndex) {
    std::lock_guard<std::mutex> lock(connMutex_);
    auto& binding = conns_[fd];
    binding.owner = subReactorIndex;
    binding.context = false;
    // 代际 0：与任何真实连接代际都不相等，事件校验统一按 fd 处理
}

void HttpServer::unregisterEventFd(int fd) {
    std::lock_guard<std::mutex> lock(connMutex_);
    conns_.erase(fd);
}

bool HttpServer::ownsConnection(int fd, int subReactorIndex, net::ConnId connId) const {
    std::lock_guard<std::mutex> lock(connMutex_);
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return false;
    }
    if (!it->second.context || it->second.owner != subReactorIndex) {
        return false;
    }
    return it->second.generation != 0 &&
           it->second.generation == net::connIdGeneration(connId);
}

bool HttpServer::fdTakenByOther(int fd, int subReactorIndex) const {
    std::lock_guard<std::mutex> lock(connMutex_);
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return false;
    }
    return it->second.owner >= 0 && it->second.owner != subReactorIndex;
}

void HttpServer::dropPendingResponses(int subReactorIndex, int fd) {
    auto& sr = subReactors_[subReactorIndex];
    std::lock_guard<std::mutex> lock(sr->wakeMutex);
    sr->pendingResponses.erase(
        std::remove_if(sr->pendingResponses.begin(), sr->pendingResponses.end(),
                       [fd](const PendingResponse& p) {
                           return net::connIdFd(p.connId) == fd;
                       }),
        sr->pendingResponses.end());
}

void HttpServer::notifyConnectionReady(int subReactorIndex, net::ConnId connId,
                                       std::shared_ptr<const std::string> data,
                                       bool keepAlive) {
    auto& sr = subReactors_[subReactorIndex];
    {
        std::lock_guard<std::mutex> lock(sr->wakeMutex);
        PendingResponse item;
        item.connId = connId;
        item.data = std::move(data);
        item.keepAlive = keepAlive;
        sr->pendingResponses.push_back(std::move(item));
    }
    uint64_t one = 1;
    // eventfd：用 MSG_NOSIGNAL 不适用（非 socket），但已全局忽略 SIGPIPE；
    // 且 eventfd 写失败只可能是关闭/EFD 计数溢出，不影响正确性。
    ssize_t n = ::write(sr->wakeFd, &one, sizeof(one));
    (void)n;
}

net::ConnId HttpServer::connIdOf(int subReactorIndex, int fd) const {
    auto& sr = subReactors_[subReactorIndex];
    std::lock_guard<std::mutex> lock(sr->contextsMutex);
    auto it = sr->contexts.find(fd);
    if (it == sr->contexts.end()) {
        return 0;
    }
    return net::makeConnId(fd, it->second->connectionGeneration());
}

HttpContext* HttpServer::lookupContext(int subReactorIndex, int fd) {
    auto& sr = subReactors_[subReactorIndex];
    std::lock_guard<std::mutex> lock(sr->contextsMutex);
    auto it = sr->contexts.find(fd);
    return (it == sr->contexts.end()) ? nullptr : it->second.get();
}

// ---- 主 Reactor（仅 accept + 分发） ----

void HttpServer::mainReactorLoop() {
    const int MAX_EVENTS = 128;
    struct epoll_event events[MAX_EVENTS];

    std::cout << "[DEBUG][HTTP服务器]：主Reactor循环启动 (仅accept)" << std::endl;

    while (running_.load()) {
        int nfds = epoll_wait(mainEpollFd_, events, MAX_EVENTS, 1000);

        if (nfds < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[ERROR][HTTP服务器]：主epoll_wait错误: " << strerror(errno) << std::endl;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = net::connIdFd(events[i].data.u64);
            if (fd == listenFd_ && (events[i].events & EPOLLIN)) {
                handleAccept();
            }
        }
    }

    std::cout << "[DEBUG][HTTP服务器]：主Reactor循环结束" << std::endl;
}

void HttpServer::handleAccept() {
    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);

    while (true) {
        clientAddrLen = sizeof(clientAddr);
        int clientFd = accept(listenFd_, (struct sockaddr*)&clientAddr, &clientAddrLen);
        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            std::cerr << "[ERROR][HTTP服务器]：accept错误: " << strerror(errno) << std::endl;
            break;
        }

        if (!setNonBlocking(clientFd)) {
            std::cerr << "[ERROR][HTTP服务器]：设置客户端socket非阻塞失败" << std::endl;
            close(clientFd);
            continue;
        }

        // 轮询选择子 Reactor
        int idx = static_cast<int>(nextSubReactor_.fetch_add(1) % subReactorCount_);
        auto& sr = subReactors_[idx];

        // fd 号可能刚从本 reactor 上一个连接回收：清掉该 fd 的陈旧待发送项，
        // 否则旧任务的完成回调会写进这个新连接（C2 的另一半）。
        dropPendingResponses(idx, clientFd);

        // 先建立连接上下文与归属记录，再注册到 epoll。
        // 反过来的话，epoll_ctl(ADD) 会在 fd 已可读时立刻回调，
        // handleRead 发现 contexts 里还没有该 fd 就会把连接关掉（C6）。
        net::ConnId connId = registerConnection(clientFd, idx);

        {
            std::lock_guard<std::mutex> lock(sr->contextsMutex);
            auto ctx = std::make_unique<HttpContext>();
            ctx->setConnectionGeneration(net::connIdGeneration(connId));
            if (useMemoryPool_) {
                ctx->enableMemoryPool(true, ownedMemoryPool_.get());
            }
            sr->contexts[clientFd] = std::move(ctx);
        }

        struct epoll_event event;
        event.events = kConnReadEvents;
        event.data.u64 = connId;

        if (epoll_ctl(sr->epollFd, EPOLL_CTL_ADD, clientFd, &event) < 0) {
            std::cerr << "[ERROR][HTTP服务器]：添加客户端到子Reactor失败, idx=" << idx
                      << " epoll: " << strerror(errno) << std::endl;
            // 回滚已插入的上下文与归属记录
            {
                std::lock_guard<std::mutex> lock(sr->contextsMutex);
                sr->contexts.erase(clientFd);
            }
            unregisterEventFd(clientFd);
            close(clientFd);
            continue;
        }

        stats_.activeConnections.fetch_add(1);

        std::cout << "[INFO][HTTP服务器]：新连接, from=" << inet_ntoa(clientAddr.sin_addr)
                  << ":" << ntohs(clientAddr.sin_port) << " (fd: " << clientFd
                  << ", reactor: " << idx << ")" << std::endl;
    }
}

// ---- 子 Reactor（read / write） ----

void HttpServer::subReactorLoop(int index) {
    const int MAX_EVENTS = 1024;
    struct epoll_event events[MAX_EVENTS];
    auto& sr = subReactors_[index];

    std::cout << "[DEBUG][HTTP服务器]：子Reactor" << index << " 循环启动" << std::endl;

    while (running_.load()) {
        int nfds = epoll_wait(sr->epollFd, events, MAX_EVENTS, 1000);

        if (nfds < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[ERROR][HTTP服务器]：子Reactor" << index << " epoll_wait error: "
                      << strerror(errno) << std::endl;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            net::ConnId connId = events[i].data.u64;
            int fd = net::connIdFd(connId);
            uint32_t ev = events[i].events;

            // eventfd 唤醒：批量处理待发送的响应
            if (fd == sr->wakeFd) {
                handleWake(index);
                continue;
            }

            // timerfd 到期：清理空闲超时连接
            if (fd == sr->timerFd) {
                handleTimer(index);
                continue;
            }

            // 代际校验：fd 号被回收再分配后，内核可能仍投递一次属于旧连接的
            // 事件；代际不符时直接丢弃，绝不按"未知连接"去关闭它。
            if (index < 0 || !ownsConnection(fd, index, connId)) {
                continue;
            }

            if (ev & (EPOLLERR | EPOLLHUP)) {
                closeConnection(fd, index);
            } else {
                if (ev & EPOLLIN)  handleRead(fd, index, connId);
                if (ev & EPOLLOUT) handleWrite(fd, index);
            }
        }
    }

    std::cout << "[DEBUG][HTTP服务器]：子Reactor" << index << " 循环结束" << std::endl;
}

void HttpServer::rearmRead(int clientFd, int subReactorIndex, net::ConnId connId) {
    auto& sr = subReactors_[subReactorIndex];
    struct epoll_event event;
    event.events = kConnReadEvents;
    event.data.u64 = connId;
    epoll_ctl(sr->epollFd, EPOLL_CTL_MOD, clientFd, &event);
}

void HttpServer::handleRead(int clientFd, int subReactorIndex, net::ConnId connId) {
    auto& sr = subReactors_[subReactorIndex];

    HttpContext* ctxPtr = nullptr;
    {
        std::lock_guard<std::mutex> lock(sr->contextsMutex);
        auto it = sr->contexts.find(clientFd);
        if (it != sr->contexts.end()) {
            ctxPtr = it->second.get();
        }
    }

    if (!ctxPtr) {
        // 上下文缺失在正常路径不可达（handleAccept 已先建上下文再注册 epoll）。
        // 这里既不知道连接状态，也不该关掉一个可能已被复用的 fd 号。
        closeConnection(clientFd, subReactorIndex);
        return;
    }

    size_t totalRead = 0;
    std::string data = readAllData(clientFd, &totalRead);

    if (totalRead == 0) {
        // 未读到任何字节：对端已关闭（EOF）或事件已被取走（EAGAIN）。
        // 用 recv 的返回语义区分，避免把"暂无数据"误判为连接终止。
        char probe = 0;
        ssize_t n = ::recv(clientFd, &probe, 1, 0);
        if (n == 0) {
            closeConnection(clientFd, subReactorIndex);
        } else {
            rearmRead(clientFd, subReactorIndex, connId);
        }
        return;
    }

    ctxPtr->touch();
    ctxPtr->appendData(data);

    // 内存池缓冲区满，请求体被截断
    if (ctxPtr->isTruncated()) {
        // 给出可区分的上限：客户端据此决定分片/重试策略，而不是拿到一个笼统的 413（H7）
        const size_t limit = ctxPtr->requestBufferCapacity();
        std::cerr << "[ERROR][HTTP服务器]：请求超过内存池单块容量, limit=" << limit
                  << " 字节" << std::endl;
        auto response = std::make_shared<HttpResponse>();
        response->setStatus(413, "Payload Too Large");
        response->setBody("{\"error\":\"Payload too large\",\"limit\":" +
                          std::to_string(limit) + "}");
        response->setHeader("Content-Type", "application/json");
        response->setHeader("Connection", "close");  // 请求被截断，不复用连接
        // 标记"响应已终结"，阻止路由覆盖该响应（H1）
        response->markFinalized();
        enqueueRequestTask(subReactorIndex, clientFd, connId,
                           std::make_shared<HttpRequest>(), response);
        return;
    }

    dispatchBufferedRequests(clientFd, subReactorIndex, connId);
}

// 解析并提交缓冲区中的完整请求（长连接下响应发完后会再次调用）
void HttpServer::dispatchBufferedRequests(int clientFd, int subReactorIndex,
                                          net::ConnId connId) {
    auto& sr = subReactors_[subReactorIndex];
    HttpContext* ctxPtr = nullptr;
    {
        std::lock_guard<std::mutex> lock(sr->contextsMutex);
        auto it = sr->contexts.find(clientFd);
        if (it != sr->contexts.end()) {
            ctxPtr = it->second.get();
        }
    }
    if (!ctxPtr) {
        closeConnection(clientFd, subReactorIndex);
        return;
    }

    // 同一连接一次只处理一个请求：响应发完后再解析下一个，保证 pipelining 的响应顺序。
    // 闸门用响应区状态（只由本线程写）而不是 worker 侧的任何字段：
    // 上一次派发的响应还没发完时不得再派发（否则响应会交错）。
    if (ctxPtr->hasCurrentResponse()) return;

    auto request = std::make_shared<HttpRequest>();
    const std::string buffered = ctxPtr->getData();
    if (!request->parse(buffered)) {
        if (!request->isContentLengthValid()) {
            // 畸形 Content-Length 必须显式拒绝：按 0 处理会让 body 被当成下一个
            // 请求解析，是请求走私面。此连接状态已不可信，响应后关闭。
            auto response = std::make_shared<HttpResponse>();
            response->setStatus(HttpStatus::BAD_REQUEST);
            response->setJson(R"({"error":"Invalid Content-Length"})");
            response->setHeader("Connection", "close");
            response->markFinalized();
            enqueueRequestTask(subReactorIndex, clientFd, connId,
                               request, response);
            return;
        }
        // 请求不完整：重新注册 EPOLLIN（EPOLLONESHOT 需要显式重置）
        rearmRead(clientFd, subReactorIndex, connId);
        return;
    }

    // 只消费"本次请求自身"占用的字节：头部结束符 + 严格按 Content-Length
    // （或 chunked 报文实际长度）切分的 body。不能用"头部之后整段缓冲区"的长度，
    // 否则同一连接上管线化的后续请求会被一并吞掉（C3），body 也会带上后续请求（C5）。
    const size_t available = buffered.size();
    size_t consumed = request->getHeaderEnd() + request->getHeaderEndSepLen() +
                      request->getBodyConsumed();
    if (consumed == 0 || consumed > available) {
        // 防御：越界消费会让解析器读空缓冲区或反复响应同一段数据
        rearmRead(clientFd, subReactorIndex, connId);
        return;
    }

    ctxPtr->consumeData(consumed);

    auto response = std::make_shared<HttpResponse>();
    enqueueRequestTask(subReactorIndex, clientFd, connId, request, response);
}

void HttpServer::enqueueRequestTask(int subReactorIndex, int clientFd, net::ConnId connId,
                                    std::shared_ptr<HttpRequest> request,
                                    std::shared_ptr<HttpResponse> response) {
    // 两条入队路径（正常解析 / 请求体截断）共用同一处记账，
    // 保证 totalRequests 与 queuedTasks 严格成对（H2）
    stats_.totalRequests.fetch_add(1);
    stats_.queuedTasks.fetch_add(1);

    // 该连接上有在途请求：空闲超时清理据此跳过它（H4）
    markInFlight(subReactorIndex, connId, /*enter=*/true);

    auto task = std::make_shared<HttpRequestTask>(
        clientFd, request, response,
        [this, subReactorIndex, connId](int fd, std::shared_ptr<HttpRequest> req,
                                        std::shared_ptr<HttpResponse> res) {
            (void)fd;
            processHttpRequest(subReactorIndex, connId, req, res);
        }
    );

    // 在途计数在"入队之前"自增：这样排队中（尚未被 worker 取走）的任务同样算在途，
    // stop() 不会在它开始执行前就放行析构（H3）。
    inFlightTasks_.fetch_add(1, std::memory_order_acq_rel);
    try {
        threadPool_->enqueue([this, subReactorIndex, connId, task]() {
            // 保证计数一定被递减：execute() 内的异常会逃到线程池的 catch，
            // 那就再也回不到 finishInFlightTask()（会把 stop() 卡到超时）
            try {
                task->execute();
            } catch (...) {
            }
            markInFlight(subReactorIndex, connId, /*enter=*/false);
            finishInFlightTask();
        });
    } catch (const std::exception& e) {
        // 线程池已关闭：撤销记账并撤销统计，不能把异常抛回 I/O 线程
        // （抛出会逃出 subReactorLoop，直接 std::terminate）
        markInFlight(subReactorIndex, connId, /*enter=*/false);
        finishInFlightTask();
        stats_.queuedTasks.fetch_sub(1);
        std::cerr << "[ERROR][HTTP服务器]：提交任务失败（线程池可能已关闭）: "
                  << e.what() << std::endl;
    }
}

// 按 (fd, 代际) 定位该连接当前的 HttpContext 并增减在途计数。
// 连接已关闭 / fd 号已被新连接复用时静默返回：计数只对"仍然存在的那条连接"有意义。
void HttpServer::markInFlight(int subReactorIndex, net::ConnId connId, bool enter) {
    if (subReactorIndex < 0 ||
        subReactorIndex >= static_cast<int>(subReactors_.size())) {
        return;
    }
    auto& sr = subReactors_[subReactorIndex];
    const int fd = net::connIdFd(connId);
    const uint32_t gen = net::connIdGeneration(connId);

    std::lock_guard<std::mutex> lock(sr->contextsMutex);
    auto it = sr->contexts.find(fd);
    if (it == sr->contexts.end()) return;
    if (it->second->connectionGeneration() != gen) return;
    if (enter) {
        it->second->enterInFlight();
    } else {
        it->second->leaveInFlight();
    }
}

void HttpServer::handleTimer(int subReactorIndex) {
    auto& sr = subReactors_[subReactorIndex];

    // 消费到期计数（timerfd 必须读取，否则会持续触发）
    uint64_t expirations = 0;
    ssize_t n = read(sr->timerFd, &expirations, sizeof(expirations));
    (void)n;

    if (idleTimeoutSec_ <= 0) return;

    const auto now = std::chrono::steady_clock::now();
    std::vector<net::ConnId> idleConns;
    size_t skippedInFlight = 0;
    {
        std::lock_guard<std::mutex> lock(sr->contextsMutex);
        for (auto& [fd, ctx] : sr->contexts) {
            // 有请求在途（handler 仍在跑或仍在队列里）：本连接不空闲。
            // 慢 handler 超过 idleTimeout 是正常的业务耗时，不能当成空闲连接杀掉（H4）。
            if (ctx->inFlightCount() > 0) {
                ++skippedInFlight;
                continue;
            }
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                            now - ctx->lastActive())
                            .count();
            if (idle >= idleTimeoutSec_) {
                idleConns.push_back(net::makeConnId(fd, ctx->connectionGeneration()));
            }
        }
    }
    (void)skippedInFlight;

    for (net::ConnId id : idleConns) {
        std::cout << "[INFO][HTTP服务器]：空闲超时关闭连接, fd=" << net::connIdFd(id)
                  << ", idle>=" << idleTimeoutSec_ << "s" << std::endl;
        closeConnectionId(id);
    }
}

void HttpServer::handleWake(int subReactorIndex) {
    auto& sr = subReactors_[subReactorIndex];

    // 消费 eventfd 的写入（防止电平触发重复唤醒）
    uint64_t val;
    ssize_t n = read(sr->wakeFd, &val, sizeof(val));
    (void)n;

    // 拉取待发送的响应（H5：worker 投递的是"连接标识 + 响应字节"）
    std::vector<PendingResponse> items;
    {
        std::lock_guard<std::mutex> lock(sr->wakeMutex);
        items.swap(sr->pendingResponses);
    }

    // 在当前 sub reactor 线程执行 send：响应区（HttpContext::currentResponse_）
    // 只有本线程触碰，worker 不再访问它
    for (auto& item : items) {
        const int fd = net::connIdFd(item.connId);
        // 陈旧投递：该 (fd, 代际) 已不再属于本 reactor（连接已关闭、fd 被新连接复用、
        // 或被空闲超时清理）→ 直接丢弃，绝不能去 close 这个 fd 号（C2）
        if (!ownsConnection(fd, subReactorIndex, item.connId)) continue;

        HttpContext* ctx = lookupContext(subReactorIndex, fd);
        if (!ctx || ctx->connectionGeneration() != net::connIdGeneration(item.connId)) {
            continue;
        }

        if (ctx->hasCurrentResponse()) {
            // 同一连接上不会有两个在途响应（派发以"响应未发完"为闸门），
            // 出现即说明状态机被破坏：保留新响应、记录，避免旧响应被截断后
            // Content-Length 与实际字节不一致
            std::cerr << "[ERROR][HTTP服务器]：连接上已有未发完的响应, fd=" << fd
                      << "（覆盖旧响应）" << std::endl;
        }
        ctx->setCurrentResponse(std::move(item.data));
        ctx->setKeepAlive(item.keepAlive);
        ctx->resetWriteOffset();
        writeCurrentResponse(fd, subReactorIndex, ctx);
    }
}

void HttpServer::handleWrite(int clientFd, int subReactorIndex) {
    HttpContext* ctxPtr = lookupContext(subReactorIndex, clientFd);
    if (!ctxPtr) {
        // 该 fd 已无上下文：可能是连接已被关闭（含 fd 被新连接复用）。
        // closeConnection 内部会做归属校验，不会动已被别的连接占用的 fd（C2）。
        closeConnection(clientFd, subReactorIndex);
        return;
    }

    if (!ctxPtr->hasCurrentResponse()) {
        // 走到这里说明 EPOLLOUT 被武装却没有待发内容（正常路径不可达：
        // EPOLLOUT 只在部分写之后挂上）。补一次 EPOLLIN，让连接自愈，
        // 不重新武装就会永久收不到事件（响应丢失）。
        rearmRead(clientFd, subReactorIndex,
                  net::makeConnId(clientFd, ctxPtr->connectionGeneration()));
        return;
    }

    writeCurrentResponse(clientFd, subReactorIndex, ctxPtr);
}

// 把 ctx 上当前响应尽量写出去；写不完就挂 EPOLLOUT 等续写。
// 只允许在 sub reactor 线程调用（响应区归 I/O 线程独有）。
void HttpServer::writeCurrentResponse(int clientFd, int subReactorIndex, HttpContext* ctxPtr) {
    auto& sr = subReactors_[subReactorIndex];

    // 持有一份 shared_ptr：resetForNextRequest() 会清掉 ctx 上的引用，
    // 但下面的发送过程仍要用这段字节
    std::shared_ptr<const std::string> respPtr = ctxPtr->currentResponse();
    if (!respPtr) return;
    const std::string& responseData = *respPtr;

    size_t offset = ctxPtr->getWriteOffset();
    size_t bytesWritten = 0;

    // write 在当前 sub reactor 线程执行，安全
    if (writeAllDataFromOffset(clientFd, responseData, offset, &bytesWritten)) {
        ctxPtr->resetWriteOffset();
        const bool keepAlive = ctxPtr->isKeepAlive();
        // 响应已完整发出，响应区可以释放（I/O 线程独占，无竞争）
        ctxPtr->clearCurrentResponse();
        if (keepAlive) {
            // 长连接：保留连接，重置上下文后继续接收同一连接的下一个请求
            ctxPtr->resetForNextRequest();
            ctxPtr->touch();
            rearmRead(clientFd, subReactorIndex,
                      net::makeConnId(clientFd, ctxPtr->connectionGeneration()));
            // 缓冲区中可能已缓存下一个请求（粘包 / pipelining），立即处理
            if (!ctxPtr->getData().empty()) {
                dispatchBufferedRequests(
                    clientFd, subReactorIndex,
                    net::makeConnId(clientFd, ctxPtr->connectionGeneration()));
            }
        } else {
            closeConnection(clientFd, subReactorIndex);
        }
    } else {
        ctxPtr->setWriteOffset(offset + bytesWritten);
        if (bytesWritten == 0) {
            // 一个字节都没写出去且不是 EAGAIN：对端已不可用（EPIPE/ECONNRESET）。
            // 继续重新武装 EPOLLOUT 只会空转，直接关闭。
            if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
                closeConnection(clientFd, subReactorIndex);
                return;
            }
        }
        // 部分写，重新注册 EPOLLOUT（仍在 sub reactor 线程，无跨线程问题）
        struct epoll_event event;
        event.events = kConnWriteEvents;
        event.data.u64 = net::makeConnId(clientFd, ctxPtr->connectionGeneration());
        epoll_ctl(sr->epollFd, EPOLL_CTL_MOD, clientFd, &event);
    }
}

void HttpServer::closeConnection(int fd, int subReactorIndex) {
    closeConnectionId(connIdOf(subReactorIndex, fd));
    // connIdOf 在上下文已不存在时返回 0：此时 fd 上已无本 reactor 的连接，
    // 不做任何关闭动作（这正是 C2 的约束：不关闭"不属于自己"的 fd）。
}

void HttpServer::closeConnectionId(net::ConnId connId) {
    if (connId == 0) return;

    int fd = net::connIdFd(connId);
    int subReactorIndex = -1;

    {
        std::lock_guard<std::mutex> lock(connMutex_);
        auto it = conns_.find(fd);
        if (it == conns_.end() || !it->second.context) {
            // 从未见过的 fd：绝不动它（可能是别的模块/别的对象的 fd）
            return;
        }
        if (it->second.generation != net::connIdGeneration(connId)) {
            // 陈旧回调：该 fd 号已被新连接占用，或已归还给其它所有者 —— 不关
            return;
        }
        subReactorIndex = it->second.owner;
        // 先注销归属（保留代际号），使重复回调无法二次关闭同一个 fd
        it->second.owner = -1;
        it->second.context = false;
    }

    if (subReactorIndex < 0 || subReactorIndex >= static_cast<int>(subReactors_.size())) {
        // 该 fd 不属于任何一个本服务器管理的连接：不动它
        return;
    }

    auto& sr = subReactors_[subReactorIndex];

    epoll_ctl(sr->epollFd, EPOLL_CTL_DEL, fd, nullptr);

    {
        std::lock_guard<std::mutex> lock(sr->contextsMutex);
        sr->contexts.erase(fd);
    }

    // 该 fd 上不再有在途响应
    dropPendingResponses(subReactorIndex, fd);

    close(fd);

    stats_.activeConnections.fetch_sub(1);

    std::cout << "[INFO][HTTP服务器]：连接关闭, fd=" << fd
              << ", reactor: " << subReactorIndex << ")" << std::endl;
}

// ---- 业务处理（线程池中执行） ----

void HttpServer::processHttpRequest(int subReactorIndex, net::ConnId connId,
                                    std::shared_ptr<HttpRequest> request,
                                    std::shared_ptr<HttpResponse> response) {
    (void)connId;  // 连接标识随响应一起投递（H5），worker 不再自己解析 fd
    try {
        if (!response->isFinalized()) {
            if (router_) {
                router_->handleRequest(*request, *response);
            } else {
                response->setStatus(HttpStatus::NOT_FOUND);
                response->setJson(R"({"error": "No router configured"})");
            }

            // 按请求的连接复用意愿回写 Connection 头，并把意愿记入上下文供 I/O 线程决策
            const bool keepAlive = request->isKeepAlive();
            response->setHeader("Connection", keepAlive ? "keep-alive" : "close");
        } else {
            // 请求体被截断等"响应已终结"的路径：保持既有语义（关闭连接）
            response->setHeader("Connection", "close");
        }

        const bool keepAlive = !response->isFinalized() && request->isKeepAlive();

        // H5：worker 只把"响应字节 + 连接标识 + 复用意愿"投递到 sub reactor 的队列，
        // 不碰 HttpContext 的任何字段 —— ctx 不再有跨线程数据。
        notifyConnectionReady(subReactorIndex, connId,
                              std::make_shared<const std::string>(response->toString()),
                              keepAlive);

        stats_.completedRequests.fetch_add(1);
        stats_.queuedTasks.fetch_sub(1);

    } catch (const std::exception& e) {
        std::cerr << "[ERROR][HTTP服务器]：请求处理错误: " << e.what() << std::endl;

        response->setStatus(HttpStatus::INTERNAL_SERVER_ERROR);
        response->setJson(R"({"error": "Internal Server Error"})");
        response->setHeader("Connection", "close");  // 出错后不复用连接

        notifyConnectionReady(subReactorIndex, connId,
                              std::make_shared<const std::string>(response->toString()),
                              /*keepAlive=*/false);

        stats_.completedRequests.fetch_add(1);
        stats_.queuedTasks.fetch_sub(1);
    }
}

// ---- I/O 辅助 ----

std::string HttpServer::readAllData(int fd, size_t* totalRead) {
    std::string data;
    char buffer[4096];
    size_t readBytes = 0;

    while (true) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            data.append(buffer, static_cast<size_t>(n));
            readBytes += static_cast<size_t>(n);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            std::cerr << "[ERROR][HTTP服务器]：读取错误: " << strerror(errno) << std::endl;
            break;
        }
    }

    if (totalRead) *totalRead = readBytes;
    return data;
}

bool HttpServer::writeAllDataFromOffset(int fd, const std::string& data,
                                         size_t offset, size_t* bytesWritten) {
    if (offset >= data.size()) {
        if (bytesWritten) *bytesWritten = 0;
        return true;
    }

    size_t totalWritten = offset;
    const char* buffer = data.c_str();
    size_t dataSize = data.size();

    while (totalWritten < dataSize) {
        // MSG_NOSIGNAL：对端 RST 后返回 EPIPE 而不是投递 SIGPIPE 杀死进程（C1）
        ssize_t n = net::sendNoSignal(fd, buffer + totalWritten, dataSize - totalWritten);
        if (n > 0) {
            totalWritten += static_cast<size_t>(n);
        } else if (n == 0) {
            if (bytesWritten) *bytesWritten = totalWritten - offset;
            return false;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (bytesWritten) *bytesWritten = totalWritten - offset;
                return false;
            }
            if (errno == EINTR) continue;
            std::cerr << "[ERROR][HTTP服务器]：写入错误: " << strerror(errno) << std::endl;
            if (bytesWritten) *bytesWritten = totalWritten - offset;
            return false;
        }
    }

    if (bytesWritten) *bytesWritten = totalWritten - offset;
    return true;
}

bool HttpServer::isPortInUse(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    int result = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    close(sock);

    return result < 0;
}

} // namespace http
