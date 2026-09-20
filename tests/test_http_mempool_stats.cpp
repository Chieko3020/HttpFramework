// test_http_mempool_stats.cpp — 内存池统计口径回归
//
// 背景：`bench_server` 的 `/stats` 曾经读进程级 `utils::GlobalMemoryPool::getInstance()`，
// 而服务实际使用的是 `HttpServer` 在 start() 时自持的池（`ownedMemoryPool_`）。
// 两者是两个不同对象，于是压测再久 `alloc_calls` 也恒为 0——"内存池默认关闭"
// 的论证一度因此失去可复现依据（L10）。
//
// 本测试锁死两条契约，防止再改回读全局单例：
//   1. `HttpServer::memoryPool()` 在未启用内存池时是 nullptr，启用后指向自持池；
//   2. `App::server()` 能取到底层 server，且经由它拿到的池的 `allocCalls()` 会随
//      真实请求增长——即统计确实接在服务正在使用的那个池上。

#include "HttpFramework.h"
#include "http/HttpServer.h"
#include "utils/MemoryPool.h"
#include "utils/ThreadPool.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while (0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)

static int g_testsPassed = 0;
static int g_testsFailed = 0;

static bool sendRequest(int port, const std::string& path) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    struct timeval tv{5, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return false;
    }
    const std::string req =
        "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ssize_t n = send(sock, req.c_str(), req.size(), 0);
    if (n <= 0) { close(sock); return false; }
    char buf[4096];
    bool got = false;
    while (recv(sock, buf, sizeof(buf), 0) > 0) got = true;
    close(sock);
    return got;
}

static bool waitPort(int port, int tries = 100) {
    for (int i = 0; i < tries; ++i) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(port));
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            const bool ok = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
            close(sock);
            if (ok) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// ── 用例 1：HttpServer 层契约 ─────────────────────────────────

static bool test_owned_pool_visibility() {
    TEST("HttpServer::memoryPool() 未启用时为 nullptr、启用后指向自持池");
    utils::ThreadPool pool(2);
    http::HttpServer server(18920, pool);

    CHECK(server.memoryPool() == nullptr, "未启用内存池时 memoryPool() 应为 nullptr");

    auto router = std::make_shared<router::Router>();
    router->get("/ping", [](const http::HttpRequest&, http::HttpResponse& res) {
        res.setText("pong");
    });
    server.setRouter(router);

    CHECK(server.start(), "服务器启动失败");
    CHECK(server.memoryPool() == nullptr, "未启用内存池时 start() 之后仍应为 nullptr");

    sendRequest(18920, "/ping");
    CHECK(server.memoryPool() == nullptr, "无池路径下 memoryPool() 不应凭空出现");

    server.stop();

    // 启用后：start() 之前还没有池（池在 start() 里创建），start() 之后有
    utils::ThreadPool pool2(2);
    http::HttpServer server2(18921, pool2);
    server2.setRouter(router);
    server2.enableMemoryPool(true);
    CHECK(server2.memoryPool() == nullptr, "池应在 start() 时创建，start() 之前为 nullptr");

    CHECK(server2.start(), "启用内存池后启动失败");
    utils::HttpMemoryPool* mp = server2.memoryPool();
    CHECK(mp != nullptr, "启用内存池后 memoryPool() 必须非空");
    CHECK(mp == server2.memoryPool(), "两次读取应是同一个池实例");

    const uint64_t before = mp->allocCalls();
    for (int i = 0; i < 20; ++i) sendRequest(18921, "/ping");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t after = mp->allocCalls();
    CHECK(after > before,
          "请求应让自持池的 allocCalls 增长——统计必须接在服务真正使用的池上（读全局单例会恒为 0）");

    server2.stop();
    PASS();
    return true;
}

// ── 用例 2：App 层转发（bench_server 的用法）──────────────────

static bool test_app_exposes_owned_pool() {
    TEST("App::server()->memoryPool() 可读，且计数随请求增长");
    http::App app;
    app.enableMemoryPool(true);
    app.get("/ping", [](const http::HttpRequest&, http::HttpResponse& res) {
        res.setText("pong");
    });

    CHECK(app.server() == nullptr, "start() 之前 App::server() 应为 nullptr");

    std::thread runner([&app] {
        try {
            app.start(18922, 2);
        } catch (const std::exception& e) {
            std::cerr << "App::start 抛异常: " << e.what() << std::endl;
        }
    });

    if (!waitPort(18922)) {
        app.stop();
        runner.join();
        FAIL("App 未能监听 18922");
    }

    auto srv = app.server();
    CHECK(srv != nullptr, "start() 之后 App::server() 不应为空");
    utils::HttpMemoryPool* mp = srv ? srv->memoryPool() : nullptr;
    CHECK(mp != nullptr, "App::enableMemoryPool 后应能读到 server 自持池");

    const uint64_t before = mp->allocCalls();
    for (int i = 0; i < 20; ++i) sendRequest(18922, "/ping");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t after = mp->allocCalls();
    CHECK(after > before, "经由 App 起服务时，自持池的 allocCalls 也应增长");

    app.stop();
    runner.join();
    PASS();
    return true;
}

int main() {
    std::cout << "=== test_http_mempool_stats ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) ++g_testsPassed;
        else ++g_testsFailed;
    };

    run(test_owned_pool_visibility,    "HttpServer 自持池可见性");
    run(test_app_exposes_owned_pool,   "App 转发自持池");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, " << g_testsFailed << " 失败"
              << "（总用例 " << (g_testsPassed + g_testsFailed) << "）" << std::endl;

    return g_testsFailed == 0 ? 0 : 1;
}
