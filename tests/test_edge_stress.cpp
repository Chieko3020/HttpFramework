// test_edge_stress.cpp — 并发压力测试
// 验证：大量 HTTP 请求无崩溃、无 fd 泄露、统计计数器正确、重启可用

#include "http/HttpServer.h"
#include "router/Router.h"
#include "utils/ThreadPool.h"
#include <iostream>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)
// SKIP：报告为"跳过"（只计入 g_testsSkipped，并置 g_skipFlag 让 run() 不要把它
//       算成通过；返回值是 false，因此退出码语义不变 —— 跳过不算失败）
#define SKIP(msg) do { std::cout << "跳过 (" << msg << ")" << std::endl; ++g_testsSkipped; g_skipFlag = true; return false; } while(0)

static int g_testsPassed = 0;
static int g_testsFailed = 0;
static int g_testsSkipped = 0;
// SKIP 用：让 run() 知道"这次返回 false 是跳过，不是失败"
[[maybe_unused]] static bool g_skipFlag = false;

static bool sendRequest(int port, const std::string& path, std::string& outResponse) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }

    std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    send(sock, request.c_str(), request.size(), 0);

    char buf[4096];
    outResponse.clear();
    while (true) {
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        outResponse += buf;
        if (outResponse.find("\r\n\r\n") != std::string::npos) break;
    }

    close(sock);
    return !outResponse.empty() && outResponse.find("200 OK") != std::string::npos;
}

static bool test_concurrent_requests() {
    TEST("1000 并发请求无崩溃");
    utils::ThreadPool pool(4);
    http::HttpServer server(18910, pool, 2);
    auto router = std::make_shared<router::Router>();
    router->get("/test", [](const http::HttpRequest&, http::HttpResponse& res) {
        res.setJson("{\"status\":\"ok\"}");
    });
    server.setRouter(router);

    if (!server.start()) {
        FAIL("服务器启动失败");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const int N = 1000;
    std::atomic<int> success{0};
    std::atomic<int> fail{0};

    auto worker = [&](int id) {
        (void)id;
        for (int i = 0; i < N / 10; ++i) {
            std::string resp;
            if (sendRequest(18910, "/test", resp)) {
                success.fetch_add(1);
            } else {
                fail.fetch_add(1);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) t.join();

    std::cout << "(成功=" << success.load() << " 失败=" << fail.load() << ") ";

    // 原实现允许 20% 失败（"success >= N*0.8"），等于给 200 次失败留了后门。
    // 环回短连接理应 100% 成功（C6 修复后 1000/1000），因此改为零容忍。
    CHECK(success.load() == N, "环回请求应全部成功, 实际 成功=" << success.load()
                               << " 失败=" << fail.load() << "/" << N);
    CHECK(fail.load() == 0, "不应有失败请求, 实际失败=" << fail.load());

    auto& stats = server.getStatistics();
    std::cout << "[请求数: " << stats.totalRequests.load()
              << " 完成数: " << stats.completedRequests.load() << "] ";
    // 服务端记账必须与客户端观测一致（H2：total 与 completed 成对）
    CHECK(stats.totalRequests.load() == static_cast<uint64_t>(success.load()),
          "服务端 totalRequests 应等于客户端成功数: " << stats.totalRequests.load()
          << " vs " << success.load());
    CHECK(stats.completedRequests.load() == stats.totalRequests.load(),
          "totalRequests 与 completedRequests 应相等: " << stats.totalRequests.load()
          << " vs " << stats.completedRequests.load());

    server.stop();
    PASS();
    return true;
}

static bool test_statistics_counters() {
    TEST("统计计数器正确增长");
    utils::ThreadPool pool(2);
    http::HttpServer server(18911, pool);
    auto router = std::make_shared<router::Router>();
    router->get("/counter", [](const http::HttpRequest&, http::HttpResponse& res) {
        res.setText("ok");
    });
    server.setRouter(router);

    if (!server.start()) {
        FAIL("服务器启动失败");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto initialTotal = server.getStatistics().totalRequests.load();
    auto initialCompleted = server.getStatistics().completedRequests.load();

    for (int i = 0; i < 10; ++i) {
        std::string resp;
        sendRequest(18911, "/counter", resp);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto finalTotal = server.getStatistics().totalRequests.load();
    auto finalCompleted = server.getStatistics().completedRequests.load();

    CHECK(finalTotal > initialTotal, "totalRequests 应增长");
    CHECK(finalCompleted > initialCompleted, "completedRequests 应增长");

    server.stop();
    PASS();
    return true;
}

static bool test_server_restart() {
    TEST("服务器停止后可重新启动");
    utils::ThreadPool pool(2);
    http::HttpServer server(18912, pool);
    auto router = std::make_shared<router::Router>();
    router->get("/hello", [](const http::HttpRequest&, http::HttpResponse& res) {
        res.setText("world");
    });
    server.setRouter(router);

    CHECK(server.start(), "首次启动应成功");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::string resp;
    CHECK(sendRequest(18912, "/hello", resp), "首次请求应成功");

    server.stop();
    CHECK(!server.isRunning(), "服务器应已停止");

    // 原实现在 start() 返回 false 时打印"不支持重启 — 可接受"就直接通过，
    // 等于"重启功能坏了也算过"。这里要求重启必须成功且能再次服务请求。
    CHECK(server.start(), "停止后重新启动必须成功（HttpServer 契约支持重启）");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::string resp2;
    CHECK(sendRequest(18912, "/hello", resp2), "重启后请求应成功");
    CHECK(resp2.find("world") != std::string::npos,
          "重启后响应体应仍然是 world, 实际: " << resp2.substr(0, 60));
    CHECK(server.isRunning(), "重启后 isRunning() 应为 true");
    server.stop();

    PASS();
    return true;
}

// 统计本进程当前打开的 fd 数（/proc/self/fd）。
// 原实现名为"fd 泄露"却完全不读 /proc —— 只验证"还能响应"，
// 因此 fd 真的泄漏时也会通过。
static int countOpenFds() {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] != '.') ++n;
    }
    closedir(d);
    return n;
}

static bool test_fd_no_leak() {
    TEST("大量连接后无 fd 泄露（按 /proc/self/fd 统计）");
    utils::ThreadPool pool(2);
    http::HttpServer server(18913, pool);
    auto router = std::make_shared<router::Router>();
    router->get("/leak", [](const http::HttpRequest&, http::HttpResponse& res) {
        res.setText("test");
    });
    server.setRouter(router);

    if (!server.start()) {
        FAIL("服务器启动失败");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 先跑 50 条预热，让 listen fd / epoll fd / 连接的稳定态建立起来
    for (int i = 0; i < 50; ++i) {
        std::string resp;
        sendRequest(18913, "/leak", resp);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const int fdBefore = countOpenFds();
    if (fdBefore < 0) {
        SKIP("/proc/self/fd 不可读，无法统计 fd");
    }

    const int N = 500;
    int failed = 0;
    for (int i = 0; i < N; ++i) {
        std::string resp;
        if (!sendRequest(18913, "/leak", resp)) ++failed;
        if (i % 100 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int fdAfter = countOpenFds();
    std::cout << "(fd: " << fdBefore << " -> " << fdAfter << ", 请求失败=" << failed << ") ";

    CHECK(failed == 0, N << " 条请求应全部成功, 实际失败 " << failed);
    CHECK(fdAfter <= fdBefore + 2,
          N << " 条请求后 fd 数应基本不变（允许 ±2 抖动）: " << fdBefore << " -> " << fdAfter);

    server.stop();
    PASS();
    return true;
}

int main() {
    std::cout << "=== test_edge_stress ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        g_skipFlag = false;   // fn() 里的 SKIP 会置位
        if (fn()) { if (!g_skipFlag) ++g_testsPassed; }
        else if (!g_skipFlag) { ++g_testsFailed; }
    };

    run(test_concurrent_requests,   "并发请求");
    run(test_statistics_counters,   "统计计数器");
    run(test_server_restart,        "服务器重启");
    run(test_fd_no_leak,            "fd 泄露检测");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败, "
              << g_testsSkipped << " 跳过（总用例 "
              << (g_testsPassed + g_testsFailed + g_testsSkipped) << "）" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
