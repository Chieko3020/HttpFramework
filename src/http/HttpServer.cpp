#include "http/HttpServer.h"
#include "router/Router.h"
#include "router/RouterHandler.h"

#include <iostream>
#include <cstring>
#include <errno.h>
#include <chrono>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

namespace http {

HttpServer::HttpServer(int port, size_t threadPoolSize, size_t subReactorCount)
    : port_(port), running_(false), listenFd_(-1), mainEpollFd_(-1),
      threadPoolSize_(threadPoolSize), useMemoryPool_(false) {

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
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    // 关闭监听 socket（唤醒主 Reactor）
    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }

    // 关闭主 epoll
    if (mainEpollFd_ >= 0) {
        close(mainEpollFd_);
        mainEpollFd_ = -1;
    }

    // 关闭所有子 epoll 和 eventfd（唤醒子 Reactor）
    for (auto& sr : subReactors_) {
        if (sr && sr->epollFd >= 0) {
            close(sr->epollFd);
            sr->epollFd = -1;
        }
        if (sr && sr->wakeFd >= 0) {
            close(sr->wakeFd);
            sr->wakeFd = -1;
        }
        if (sr && sr->timerFd >= 0) {
            close(sr->timerFd);
            sr->timerFd = -1;
        }
    }

    // 等待主 Reactor 线程
    if (mainReactorThread_.joinable()) {
        mainReactorThread_.join();
    }

    // 等待所有子 Reactor 线程
    for (auto& sr : subReactors_) {
        if (sr && sr->thread.joinable()) {
            sr->thread.join();
        }
    }

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

    // 清理 fd→reactor 映射
    {
        std::lock_guard<std::mutex> lock(fdToReactorMutex_);
        fdToReactor_.clear();
    }

    std::cout << "[INFO][HTTP服务器]：服务已停止" << std::endl;
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
    event.data.fd = listenFd_;

    if (epoll_ctl(mainEpollFd_, EPOLL_CTL_ADD, listenFd_, &event) < 0) {
        std::cerr << "[ERROR][HTTP服务器]：添加监听socket到主epoll失败: " << strerror(errno) << std::endl;
        close(mainEpollFd_);
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
    ev.data.fd = sr->wakeFd;
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
    ev.data.fd = sr->timerFd;
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

void HttpServer::enableMemoryPool(bool enable) {
    useMemoryPool_ = enable;
    std::cout << "[INFO][HTTP服务器]：内存池已" << (enable ? "启用" : "禁用") << std::endl;
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
            if (events[i].data.fd == listenFd_ && (events[i].events & EPOLLIN)) {
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

        // 添加到子 Reactor 的 epoll
        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        event.data.fd = clientFd;

        if (epoll_ctl(sr->epollFd, EPOLL_CTL_ADD, clientFd, &event) < 0) {
            std::cerr << "[ERROR][HTTP服务器]：添加客户端到子Reactor失败, idx=" << idx
                      << " epoll: " << strerror(errno) << std::endl;
            close(clientFd);
            continue;
        }

        // 创建上下文
        {
            std::lock_guard<std::mutex> lock(sr->contextsMutex);
            sr->contexts[clientFd] = std::make_unique<HttpContext>();
            if (useMemoryPool_) {
                sr->contexts[clientFd]->enableMemoryPool(true);
            }
        }

        // 记录 fd→reactor 映射
        {
            std::lock_guard<std::mutex> lock(fdToReactorMutex_);
            fdToReactor_[clientFd] = idx;
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
            int fd = events[i].data.fd;
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

            if (ev & (EPOLLERR | EPOLLHUP)) {
                closeConnection(fd, index);
            } else {
                if (ev & EPOLLIN)  handleRead(fd, index);
                if (ev & EPOLLOUT) handleWrite(fd, index);
            }
        }
    }

    std::cout << "[DEBUG][HTTP服务器]：子Reactor" << index << " 循环结束" << std::endl;
}

void HttpServer::handleRead(int clientFd, int subReactorIndex) {
    std::string data = readAllData(clientFd);

    if (data.empty()) {
        closeConnection(clientFd, subReactorIndex);
        return;
    }

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

    ctxPtr->touch();
    ctxPtr->appendData(data);

    // 内存池缓冲区满，请求体被截断
    if (ctxPtr->isTruncated()) {
        std::cerr << "[ERROR][HTTP服务器]：请求体超过内存池缓冲区上限" << std::endl;
        auto response = std::make_shared<HttpResponse>();
        response->setStatus(413, "Payload Too Large");
        response->setBody("{\"error\":\"Payload too large\"}");
        response->setHeader("Content-Type", "application/json");
        response->setHeader("Connection", "close");  // 请求被截断，不复用连接
        auto task = std::make_shared<HttpRequestTask>(
            clientFd, std::make_shared<HttpRequest>(), response,
            [this, subReactorIndex](int fd, std::shared_ptr<HttpRequest> req,
                                     std::shared_ptr<HttpResponse> res) {
                processHttpRequest(subReactorIndex, fd, req, res);
            }
        );
        threadPool_->enqueue([task]() {
            task->execute();
        });
        return;
    }

    dispatchBufferedRequests(clientFd, subReactorIndex);
}

// 解析并提交缓冲区中的完整请求（长连接下响应发完后会再次调用）
void HttpServer::dispatchBufferedRequests(int clientFd, int subReactorIndex) {
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

    // 同一连接一次只处理一个请求：响应发完后再解析下一个，保证 pipelining 的响应顺序
    if (!ctxPtr->getResponseData().empty()) return;

    auto request = std::make_shared<HttpRequest>();
    if (!request->parse(ctxPtr->getData())) {
        // 请求不完整：重新注册 EPOLLIN（EPOLLONESHOT 需要显式重置）
        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        event.data.fd = clientFd;
        epoll_ctl(sr->epollFd, EPOLL_CTL_MOD, clientFd, &event);
        return;
    }

    // 只消费已解析的请求数据，保留缓冲区中可能的下一个请求（粘包 / pipelining）
    size_t consumed = request->getHeaderEnd() + request->getHeaderEndSepLen();
    size_t contentLength = request->getContentLength();
    consumed += (contentLength > 0) ? contentLength : request->getRawBodySize();

    if (consumed == 0) {
        // 防御：解析结果未能定位头部结束位置时按"请求不完整"处理，避免反复响应同一段数据
        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        event.data.fd = clientFd;
        epoll_ctl(sr->epollFd, EPOLL_CTL_MOD, clientFd, &event);
        return;
    }

    ctxPtr->consumeData(consumed);

    auto response = std::make_shared<HttpResponse>();
    auto task = std::make_shared<HttpRequestTask>(
        clientFd, request, response,
        [this, subReactorIndex](int fd, std::shared_ptr<HttpRequest> req,
                                 std::shared_ptr<HttpResponse> res) {
            processHttpRequest(subReactorIndex, fd, req, res);
        }
    );

    threadPool_->enqueue([task]() {
        task->execute();
    });

    stats_.totalRequests.fetch_add(1);
    stats_.queuedTasks.fetch_add(1);
}

void HttpServer::handleTimer(int subReactorIndex) {
    auto& sr = subReactors_[subReactorIndex];

    // 消费到期计数（timerfd 必须读取，否则会持续触发）
    uint64_t expirations = 0;
    ssize_t n = read(sr->timerFd, &expirations, sizeof(expirations));
    (void)n;

    if (idleTimeoutSec_ <= 0) return;

    const auto now = std::chrono::steady_clock::now();
    std::vector<int> idleFds;
    {
        std::lock_guard<std::mutex> lock(sr->contextsMutex);
        for (auto& [fd, ctx] : sr->contexts) {
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                            now - ctx->lastActive())
                            .count();
            if (idle >= idleTimeoutSec_) idleFds.push_back(fd);
        }
    }

    for (int fd : idleFds) {
        std::cout << "[INFO][HTTP服务器]：空闲超时关闭连接, fd=" << fd
                  << ", idle>=" << idleTimeoutSec_ << "s" << std::endl;
        closeConnection(fd, subReactorIndex);
    }
}

void HttpServer::handleWake(int subReactorIndex) {
    auto& sr = subReactors_[subReactorIndex];

    // 消费 eventfd 的写入（防止电平触发重复唤醒）
    uint64_t val;
    read(sr->wakeFd, &val, sizeof(val));

    // 拉取待发送的 fd 列表
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(sr->wakeMutex);
        fds.swap(sr->pendingWrites);
    }

    // 在当前 sub reactor 线程执行 send
    for (int fd : fds) {
        handleWrite(fd, subReactorIndex);
    }
}

void HttpServer::handleWrite(int clientFd, int subReactorIndex) {
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

    std::string responseData = ctxPtr->getResponseData();
    if (!responseData.empty()) {
        size_t offset = ctxPtr->getWriteOffset();
        size_t bytesWritten = 0;

        // write 在当前 sub reactor 线程执行，安全
        if (writeAllDataFromOffset(clientFd, responseData, offset, &bytesWritten)) {
            ctxPtr->resetWriteOffset();
            if (ctxPtr->isKeepAlive()) {
                // 长连接：保留连接，重置上下文后继续接收同一连接的下一个请求
                ctxPtr->resetForNextRequest();
                ctxPtr->touch();
                struct epoll_event event;
                event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                event.data.fd = clientFd;
                epoll_ctl(sr->epollFd, EPOLL_CTL_MOD, clientFd, &event);
                // 缓冲区中可能已缓存下一个请求（粘包 / pipelining），立即处理
                if (!ctxPtr->getData().empty()) {
                    dispatchBufferedRequests(clientFd, subReactorIndex);
                }
            } else {
                closeConnection(clientFd, subReactorIndex);
            }
        } else {
            ctxPtr->setWriteOffset(offset + bytesWritten);
            // 部分写，重新注册 EPOLLOUT（仍在 sub reactor 线程，无跨线程问题）
            struct epoll_event event;
            event.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
            event.data.fd = clientFd;
            epoll_ctl(sr->epollFd, EPOLL_CTL_MOD, clientFd, &event);
        }
    }
}

void HttpServer::closeConnection(int fd, int subReactorIndex) {
    auto& sr = subReactors_[subReactorIndex];

    epoll_ctl(sr->epollFd, EPOLL_CTL_DEL, fd, nullptr);

    {
        std::lock_guard<std::mutex> lock(sr->contextsMutex);
        sr->contexts.erase(fd);
    }

    {
        std::lock_guard<std::mutex> lock(fdToReactorMutex_);
        fdToReactor_.erase(fd);
    }

    close(fd);

    stats_.activeConnections.fetch_sub(1);

    std::cout << "[INFO][HTTP服务器]：连接关闭, fd=" << fd
              << ", reactor: " << subReactorIndex << ")" << std::endl;
}

// ---- 业务处理（线程池中执行） ----

void HttpServer::processHttpRequest(int subReactorIndex, int clientFd,
                                    std::shared_ptr<HttpRequest> request,
                                    std::shared_ptr<HttpResponse> response) {
    try {
        if (router_) {
            router_->handleRequest(*request, *response);
        } else {
            response->setStatus(HttpStatus::NOT_FOUND);
            response->setJson(R"({"error": "No router configured"})");
        }

        // 按请求的连接复用意愿回写 Connection 头，并把意愿记入上下文供 I/O 线程决策
        const bool keepAlive = request->isKeepAlive();
        response->setHeader("Connection", keepAlive ? "keep-alive" : "close");

        std::string responseData = response->toString();
        auto& sr = subReactors_[subReactorIndex];

        {
            std::lock_guard<std::mutex> lock(sr->contextsMutex);
            auto it = sr->contexts.find(clientFd);
            if (it != sr->contexts.end()) {
                it->second->setResponseData(responseData);
                it->second->setKeepAlive(keepAlive);
            }
        }

        // 通过 eventfd 唤醒 sub reactor 线程执行 send，避免跨线程 epoll_ctl 竞态
        {
            std::lock_guard<std::mutex> lock(sr->wakeMutex);
            sr->pendingWrites.push_back(clientFd);
        }
        uint64_t one = 1;
        write(sr->wakeFd, &one, sizeof(one));

        stats_.completedRequests.fetch_add(1);
        stats_.queuedTasks.fetch_sub(1);

    } catch (const std::exception& e) {
        std::cerr << "[ERROR][HTTP服务器]：请求处理错误: " << e.what() << std::endl;

        auto& sr = subReactors_[subReactorIndex];

        response->setStatus(HttpStatus::INTERNAL_SERVER_ERROR);
        response->setJson(R"({"error": "Internal Server Error"})");
        response->setHeader("Connection", "close");  // 出错后不复用连接

        std::string errData = response->toString();
        {
            std::lock_guard<std::mutex> lock(sr->contextsMutex);
            auto it = sr->contexts.find(clientFd);
            if (it != sr->contexts.end()) {
                it->second->setResponseData(errData);
                it->second->setKeepAlive(false);
            }
        }

        // 通过 eventfd 唤醒 sub reactor 线程执行 send
        {
            std::lock_guard<std::mutex> lock(sr->wakeMutex);
            sr->pendingWrites.push_back(clientFd);
        }
        uint64_t one = 1;
        write(sr->wakeFd, &one, sizeof(one));

        stats_.queuedTasks.fetch_sub(1);
    }
}

// ---- I/O 辅助 ----

std::string HttpServer::readAllData(int fd) {
    std::string data;
    char buffer[4096];

    while (true) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            data.append(buffer, n);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            std::cerr << "[ERROR][HTTP服务器]：读取错误: " << strerror(errno) << std::endl;
            break;
        }
    }

    return data;
}

bool HttpServer::writeAllData(int fd, const std::string& data) {
    return writeAllDataFromOffset(fd, data, 0);
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
        ssize_t n = write(fd, buffer + totalWritten, dataSize - totalWritten);
        if (n > 0) {
            totalWritten += n;
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
