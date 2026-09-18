// test_http_keepalive.cpp — HTTP 长连接与空闲超时验收测试
//
// 验收项：
//   ① 同一连接连续多个请求均正确响应（复用生效，响应带 Connection: keep-alive）
//   ② 请求头 Connection: close 时服务端关闭连接
//   ③ HTTP/1.0 无 keep-alive 声明时服务端关闭连接（默认短连接）
//   ④ 空闲超过阈值后服务端主动关闭连接
//   ⑤ 半包请求补齐后仍能正确解析，且连接可继续复用（状态重置干净）

#include "http/HttpServer.h"
#include "router/Router.h"
#include "utils/ThreadPool.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while (0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)
// SKIP：报告为"跳过"（只计入 g_testsSkipped，并置 g_skipFlag 让 run() 不要把它
//       算成通过；返回值是 false，因此退出码语义不变 —— 跳过不算失败）
#define SKIP(msg) do { std::cout << "跳过 (" << msg << ")" << std::endl; ++g_testsSkipped; g_skipFlag = true; return false; } while(0)

static int g_testsPassed = 0;
static int g_testsFailed = 0;
static int g_testsSkipped = 0;
// SKIP 用：让 run() 知道"这次返回 false 是跳过，不是失败"
[[maybe_unused]] static bool g_skipFlag = false;

static void report(bool ok) { if (ok) ++g_testsPassed; else ++g_testsFailed; }

static int connectTo(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct timeval tv;
    tv.tv_sec = 3;
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
        return -1;
    }
    return sock;
}

// 读取一个完整响应（依据 Content-Length 判断边界）
static bool readOneResponse(int sock, std::string& out) {
    out.clear();
    char buf[4096];
    while (true) {
        auto hdrEnd = out.find("\r\n\r\n");
        if (hdrEnd != std::string::npos) {
            size_t cl = 0;
            auto pos = out.find("Content-Length:");
            if (pos != std::string::npos && pos < hdrEnd) {
                try {
                    cl = std::stoul(out.substr(pos + 15));
                } catch (...) {
                    cl = 0;
                }
            }
            if (out.size() >= hdrEnd + 4 + cl) return true;
        }
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        out.append(buf, static_cast<size_t>(n));
    }
}

// 对端是否已关闭连接（读到 FIN 返回 true）
static bool isClosedByPeer(int sock) {
    char buf[512];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    return n == 0;
}

static bool sendAll(int sock, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(sock, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// ── ① 连接复用 ──
static bool test_keepalive_reuse(int port) {
    TEST("① 同一连接连续 3 个请求均正确响应");
    int s = connectTo(port);
    CHECK(s >= 0, "连接失败");

    for (int i = 0; i < 3; ++i) {
        std::string req = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
        CHECK(sendAll(s, req), "发送请求失败");
        std::string resp;
        CHECK(readOneResponse(s, resp), "未收到响应");
        CHECK(resp.rfind("HTTP/1.1 200", 0) == 0, "状态行应以 HTTP/1.1 200 开头");
        CHECK(resp.find("Connection: keep-alive") != std::string::npos,
              "响应应包含 Connection: keep-alive");
    }
    close(s);
    PASS();
    return true;
}

// ── ② Connection: close ──
static bool test_connection_close(int port) {
    TEST("② Connection: close 时服务端关闭连接");
    int s = connectTo(port);
    CHECK(s >= 0, "连接失败");

    std::string req = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    CHECK(sendAll(s, req), "发送请求失败");

    std::string resp;
    CHECK(readOneResponse(s, resp), "未收到响应");
    CHECK(resp.find("Connection: close") != std::string::npos,
          "响应应包含 Connection: close");
    CHECK(isClosedByPeer(s), "服务端应关闭连接");
    close(s);
    PASS();
    return true;
}

// ── ③ HTTP/1.0 默认短连接 ──
static bool test_http10_default_close(int port) {
    TEST("③ HTTP/1.0 未声明 keep-alive 时服务端关闭连接");
    int s = connectTo(port);
    CHECK(s >= 0, "连接失败");

    std::string req = "GET /ping HTTP/1.0\r\nHost: localhost\r\n\r\n";
    CHECK(sendAll(s, req), "发送请求失败");

    std::string resp;
    CHECK(readOneResponse(s, resp), "未收到响应");
    CHECK(resp.rfind("HTTP/1.1 200", 0) == 0, "状态行应以 HTTP/1.1 200 开头");
    CHECK(isClosedByPeer(s), "HTTP/1.0 默认应为短连接，服务端应关闭");
    close(s);
    PASS();
    return true;
}

// ── ④ 空闲超时 ──
static bool test_idle_timeout(int port) {
    TEST("④ 空闲超时后服务端关闭连接（阈值 2s）");
    int s = connectTo(port);
    CHECK(s >= 0, "连接失败");

    // 不发任何请求，等待超过阈值（timerfd 每 5s 检查一次，留足余量）
    std::this_thread::sleep_for(std::chrono::seconds(9));
    CHECK(isClosedByPeer(s), "空闲超时后服务端应关闭连接");
    close(s);
    PASS();
    return true;
}

// ── ⑤ 半包补齐后复用 ──
static bool test_partial_then_reuse(int port) {
    TEST("⑤ 半包补齐后正确解析，且连接可继续复用");
    int s = connectTo(port);
    CHECK(s >= 0, "连接失败");

    // 先发请求的前半段（不含结束的 CRLF CRLF）
    CHECK(sendAll(s, "GET /ping HTTP/1.1\r\nHost: local"), "发送前半段失败");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    // 再补后半段
    CHECK(sendAll(s, "host\r\n\r\n"), "发送后半段失败");

    std::string resp;
    CHECK(readOneResponse(s, resp), "半包补齐后未收到响应");
    CHECK(resp.rfind("HTTP/1.1 200", 0) == 0, "半包补齐后状态行应为 200");

    // 同一连接继续发一个完整请求，验证状态已被干净重置
    std::string req2 = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    CHECK(sendAll(s, req2), "复用发送失败");
    std::string resp2;
    CHECK(readOneResponse(s, resp2), "复用后未收到响应");
    CHECK(resp2.rfind("HTTP/1.1 200", 0) == 0, "复用后状态行应为 200");
    CHECK(resp2.find("pong") != std::string::npos, "复用后响应体应正确");

    close(s);
    PASS();
    return true;
}

int main() {
    std::cout << "=== HTTP 长连接与空闲超时测试 ===" << std::endl;

    utils::ThreadPool pool(4);

    // 主实例：默认 60s 空闲超时
    http::HttpServer server(18920, pool, 2);
    auto router = std::make_shared<router::Router>();
    router->get("/ping", [](const http::HttpRequest&, http::HttpResponse& res) {
        res.setHeader("Content-Type", "text/plain");
        res.setBody("pong");
    });
    server.setRouter(router);

    if (!server.start()) {
        std::cerr << "服务启动失败" << std::endl;
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    report(test_keepalive_reuse(18920));
    report(test_connection_close(18920));
    report(test_http10_default_close(18920));
    report(test_partial_then_reuse(18920));

    server.stop();

    // 超时实例：2s 空闲超时
    http::HttpServer idleServer(18921, pool, 1);
    idleServer.setIdleTimeout(2);
    auto router2 = std::make_shared<router::Router>();
    router2->get("/ping", [](const http::HttpRequest&, http::HttpResponse& res) {
        res.setBody("pong");
    });
    idleServer.setRouter(router2);
    if (!idleServer.start()) {
        std::cerr << "超时实例启动失败" << std::endl;
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    report(test_idle_timeout(18921));
    idleServer.stop();

    std::cout << "\n结果: " << g_testsPassed << " 通过, " << g_testsFailed << " 失败, "
              << g_testsSkipped << " 跳过"
              << std::endl;
    return g_testsFailed == 0 ? 0 : 1;
}
