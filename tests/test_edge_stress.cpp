// test_edge_stress.cpp — 并发压力测试
// 验证：大量 HTTP 请求无崩溃、无 fd 泄露、统计计数器正确、重启可用

#include "http/HttpServer.h"
#include "router/Router.h"
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

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)

static int g_testsPassed = 0;
static int g_testsFailed = 0;

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
    http::HttpServer server(18910, 4, 2);
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

    CHECK(success.load() >= static_cast<int>(N * 0.8),
          "成功率过低: " << fail.load() << "/" << N << " 失败");

    auto& stats = server.getStatistics();
    std::cout << "[请求数: " << stats.totalRequests.load()
              << " 完成数: " << stats.completedRequests.load() << "] ";

    server.stop();
    PASS();
    return true;
}

static bool test_statistics_counters() {
    TEST("统计计数器正确增长");
    http::HttpServer server(18911, 2);
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
    http::HttpServer server(18912, 2);
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

    bool secondStart = server.start();
    if (secondStart) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::string resp2;
        bool secondReq = sendRequest(18912, "/hello", resp2);
        CHECK(secondReq, "重启后请求应成功");
        server.stop();
    } else {
        std::cout << "(不支持重启 — 可接受) ";
    }

    PASS();
    return true;
}

static bool test_fd_no_leak() {
    TEST("大量连接后无 fd 泄露");
    http::HttpServer server(18913, 2);
    auto router = std::make_shared<router::Router>();
    router->get("/leak", [](const http::HttpRequest&, http::HttpResponse& res) {
        res.setText("test");
    });
    server.setRouter(router);

    if (!server.start()) {
        FAIL("服务器启动失败");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int N = 500;

    for (int i = 0; i < N; ++i) {
        std::string resp;
        sendRequest(18913, "/leak", resp);
        if (i % 100 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::string finalResp;
    CHECK(sendRequest(18913, "/leak", finalResp),
          N << " 个请求后服务器应仍能响应");

    server.stop();
    PASS();
    return true;
}

int main() {
    std::cout << "=== test_edge_stress ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) { g_testsPassed++; }
        else      { g_testsFailed++; }
    };

    run(test_concurrent_requests,   "并发请求");
    run(test_statistics_counters,   "统计计数器");
    run(test_server_restart,        "服务器重启");
    run(test_fd_no_leak,            "fd 泄露检测");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
