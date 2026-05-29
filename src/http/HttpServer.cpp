#include "http/HttpServer.h"
#include "router/Router.h"
#include "router/RouterHandler.h"

#include <iostream>
#include <cstring>
#include <errno.h>
#include <chrono>

namespace http {

HttpServer::HttpServer(int port, size_t threadPoolSize, size_t subReactorCount)
    : port_(port), running_(false), listenFd_(-1), mainEpollFd_(-1), useMemoryPool_(false) {

    if (subReactorCount == 0)
        subReactorCount = std::thread::hardware_concurrency();
    if (subReactorCount < 1)
        subReactorCount = 1;
    subReactorCount_ = subReactorCount;

    threadPool_ = new utils::ThreadPool(threadPoolSize);
    ownsThreadPool_ = true;

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
    ownsThreadPool_ = false;

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

    // 关闭所有子 epoll（唤醒子 Reactor）
    for (auto& sr : subReactors_) {
        if (sr && sr->epollFd >= 0) {
            close(sr->epollFd);
            sr->epollFd = -1;
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
    if (threadPool_ && ownsThreadPool_) {
        threadPool_->shutdown();
        delete threadPool_;
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

    if (setsockopt(listenFd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        std::cerr << "[ERROR][HTTP服务器]：设置SO_REUSEPORT失败: " << strerror(errno) << std::endl;
        close(listenFd_);
        return false;
    }

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

    ctxPtr->appendData(data);

    auto request = std::make_shared<HttpRequest>();
    if (request->parse(ctxPtr->getData())) {
        auto response = std::make_shared<HttpResponse>();
        ctxPtr->clearData();

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
    } else {
        // 请求不完整，重新启用 EPOLLIN（EPOLLONESHOT 模式需要）
        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        event.data.fd = clientFd;
        epoll_ctl(sr->epollFd, EPOLL_CTL_MOD, clientFd, &event);
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

    const std::string& responseData = ctxPtr->getResponseData();
    if (!responseData.empty()) {
        size_t offset = ctxPtr->getWriteOffset();
        size_t bytesWritten = 0;

        if (writeAllDataFromOffset(clientFd, responseData, offset, &bytesWritten)) {
            ctxPtr->resetWriteOffset();
            closeConnection(clientFd, subReactorIndex);
        } else {
            ctxPtr->setWriteOffset(offset + bytesWritten);
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

        std::string responseData = response->toString();
        auto& sr = subReactors_[subReactorIndex];

        {
            std::lock_guard<std::mutex> lock(sr->contextsMutex);
            auto it = sr->contexts.find(clientFd);
            if (it != sr->contexts.end()) {
                it->second->setResponseData(responseData);
            }
        }

        struct epoll_event event;
        event.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
        event.data.fd = clientFd;
        epoll_ctl(sr->epollFd, EPOLL_CTL_MOD, clientFd, &event);

        stats_.completedRequests.fetch_add(1);
        stats_.queuedTasks.fetch_sub(1);

    } catch (const std::exception& e) {
        std::cerr << "[ERROR][HTTP服务器]：请求处理错误: " << e.what() << std::endl;

        auto& sr = subReactors_[subReactorIndex];

        response->setStatus(HttpStatus::INTERNAL_SERVER_ERROR);
        response->setJson(R"({"error": "Internal Server Error"})");

        std::string errData = response->toString();
        {
            std::lock_guard<std::mutex> lock(sr->contextsMutex);
            auto it = sr->contexts.find(clientFd);
            if (it != sr->contexts.end()) {
                it->second->setResponseData(errData);
            }
        }

        struct epoll_event event;
        event.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
        event.data.fd = clientFd;
        epoll_ctl(sr->epollFd, EPOLL_CTL_MOD, clientFd, &event);

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
