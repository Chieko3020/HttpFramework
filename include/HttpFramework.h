#pragma once

#include "http/HttpServer.h"
#include "router/Router.h"
#include "session/SessionManager.h"
#include "middleware/SessionMiddleware.h"
#include "utils/db/DbConnectionPool.h"
#include "utils/ThreadPool.h"
#include "utils/Logger.h"

#include <signal.h>
#include <atomic>
#include <iostream>
#include <thread>
#include <chrono>

namespace http {

class App {
public:
    using Handler    = router::Router::Handler;
    using Middleware = router::Router::Middleware;

    App() {
        router_ = std::make_shared<router::Router>();
        App* old = s_current;
        s_current = this;
        if (!old) {
            signal(SIGINT,  App::onSignal);
            signal(SIGTERM, App::onSignal);
        }
    }

    ~App() {
        stop();
        if (s_current == this) s_current = nullptr;
    }

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // ---- 路由 (委托给 Router) ----

    App& get(const std::string& path, Handler h)        { router_->get(path, std::move(h));     return *this; }
    App& post(const std::string& path, Handler h)       { router_->post(path, std::move(h));    return *this; }
    App& put(const std::string& path, Handler h)        { router_->put(path, std::move(h));     return *this; }
    App& del(const std::string& path, Handler h)        { router_->del(path, std::move(h));     return *this; }
    App& patch(const std::string& path, Handler h)      { router_->patch(path, std::move(h));   return *this; }
    App& head(const std::string& path, Handler h)       { router_->head(path, std::move(h));    return *this; }
    App& options(const std::string& path, Handler h)    { router_->options(path, std::move(h)); return *this; }

    App& use(Middleware m)                              { router_->use(std::move(m));           return *this; }
    App& use(const std::string& path, Middleware m)     { router_->use(path, std::move(m));     return *this; }

    App& notFound(Handler h)                            { router_->setNotFoundHandler(std::move(h)); return *this; }

    // ---- 中间件 ----

    App& enableLogging() {
        router_->use([](const http::HttpRequest& req, http::HttpResponse&, std::function<void()> next) {
            LOG_INFO("HTTP", req.getMethodString() << " " << req.getPath());
            next();
        });
        return *this;
    }

    App& enableSession(int expirationSeconds = 1800, int cleanupIntervalSeconds = 300) {
        if (!sessionMgr_) {
            sessionMgr_ = std::make_shared<session::SessionManager>(std::chrono::seconds(expirationSeconds));
            sessionMgr_->startCleanupThread(std::chrono::seconds(cleanupIntervalSeconds));
        }
        router_->use(middleware::session_utils::createSessionMiddleware(sessionMgr_));
        return *this;
    }

    App& enableMemoryPool(bool enable = true) {
        useMemoryPool_ = enable;
        return *this;
    }

    // ---- 数据库 (可选) ----

    App& enableDatabase(const std::string& host, const std::string& user,
                        const std::string& password, const std::string& database,
                        int port = 3306, int maxConnections = 5) {
        dbPool_ = std::make_shared<db::DbConnectionPool>(host, user, password, database, port, maxConnections);
        if (!dbPool_->initialize()) {
            LOG_WARN("HTTP框架", "数据库连接池初始化失败, 数据库功能已禁用");
            dbPool_.reset();
        }
        return *this;
    }

    // ---- 启动 & 停止 ----

    void start(int port, int threads = 4) {
        // 创建共享线程池
        auto pool = std::make_shared<utils::ThreadPool>(
            threads > 0 ? static_cast<size_t>(threads)
                        : std::thread::hardware_concurrency());
        appThreadPool_ = pool;

        server_ = std::make_shared<http::HttpServer>(port, *pool);
        server_->setRouter(router_);
        if (useMemoryPool_) server_->enableMemoryPool(true);

        LOG_INFO("HTTP框架", "HTTP服务启动, http://localhost:" << port);

        if (!server_->start()) {
            throw std::runtime_error("Failed to start server on port " + std::to_string(port));
        }

        // 主循环同时负责"信号 → 关闭"：信号处理器只置标志（async-signal-safe），
        // 日志与关闭流程都在主线程完成（H15）。
        while (server_->isRunning()

              ) {
            if (shutdownRequested()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        const int sig = s_shutdownSignal.load(std::memory_order_relaxed);
        if (sig != 0) {
            s_shutdownSignal.store(0, std::memory_order_relaxed);
            LOG_INFO("HTTP框架", "收到关闭信号 (signal=" << sig << ")，开始关闭");
        }

        server_->stop();

        if (sessionMgr_) sessionMgr_->stopCleanupThread();
        pool->shutdown();
    }

    void stop() {
        if (server_ && server_->isRunning())
            server_->stop();

    }

    // ---- 访问底层对象 ----

    std::shared_ptr<router::Router>           router()         { return router_; }
    std::shared_ptr<session::SessionManager>  sessionManager() { return sessionMgr_; }
    std::shared_ptr<db::DbConnectionPool>     dbPool()         { return dbPool_; }

    // 底层 HttpServer（start() 之前为 nullptr）。用于读取服务自持的运行时资源，
    // 例如内存池统计：`app.server()->memoryPool()`——统计必须走这里，
    // 读进程级 GlobalMemoryPool 会永远得到 0（L10）。
    std::shared_ptr<http::HttpServer>         server()         { return server_; }

    // 真实统计：转发到 HttpServer 的统计对象。
    // 此前返回的是 App 自己那个从未被写入的 stats_，读出来恒为 0（H15）。
    // 未 start() 时返回零值统计（成员 stats_）。
    const http::HttpServer::Statistics&       stats()          { return server_ ? server_->getStatistics() : stats_; }

    // 是否已收到 SIGINT/SIGTERM（由主循环轮询）
    static bool shutdownRequested() {
        return s_shutdownSignal.load(std::memory_order_relaxed) != 0;
    }

private:
    std::shared_ptr<router::Router>           router_;
    std::shared_ptr<http::HttpServer>         server_;
    std::shared_ptr<session::SessionManager>  sessionMgr_;
    std::shared_ptr<db::DbConnectionPool>     dbPool_;
    std::shared_ptr<utils::ThreadPool>        appThreadPool_;
    http::HttpServer::Statistics              stats_;
    bool useMemoryPool_ = false;

    static inline App* s_current = nullptr;
    // 关闭信号（SIGINT/SIGTERM）：信号处理器只允许做异步信号安全的事，
    // 因此这里只存一个标志，日志与关闭流程交给主循环（H15）。
    static inline std::atomic<int> s_shutdownSignal{0};

    static void onSignal(int sig) {
        s_shutdownSignal.store(sig, std::memory_order_relaxed);
    }
};

} // namespace http
