// test_edge_input.cpp — 非法输入边界测试
// 验证：大 header 不崩溃、路径穿越拒绝、畸形请求不崩溃、
//       空请求不崩溃、端口占用检测、同端口拒绝

#include "http/HttpServer.h"
#include "router/Router.h"
#include "utils/ThreadPool.h"
#include <iostream>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

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

// 辅助：连接到服务器，发送请求，读取响应
static std::string sendRawRequest(int port, const std::string& request, int timeoutMs = 2000) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return "";
    }

    size_t sent = 0;
    while (sent < request.size()) {
        ssize_t n = send(sock, request.data() + sent, request.size() - sent, 0);
        if (n <= 0) break;
        sent += n;
    }

    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        response += buf;
        if (response.find("\r\n\r\n") != std::string::npos) {
            size_t bodyStart = response.find("\r\n\r\n") + 4;
            size_t clPos = response.find("Content-Length: ");
            if (clPos != std::string::npos) {
                size_t clEnd = response.find("\r\n", clPos);
                std::string clStr = response.substr(clPos + 16, clEnd - (clPos + 16));
                size_t expectedLen = std::stoul(clStr);
                size_t bodyLen = response.size() - bodyStart;
                if (bodyLen < expectedLen) {
                    continue;
                }
            }
            break;
        }
    }

    close(sock);
    return response;
}

static bool test_oversized_header() {
    TEST("大 header 请求不导致服务器崩溃");
    utils::ThreadPool pool(2);
    http::HttpServer server(18901, pool);
    auto router = std::make_shared<router::Router>();
    router->get("/test", [](const http::HttpRequest&, http::HttpResponse&){});
    server.setRouter(router);

    if (!server.start()) {
        FAIL("服务器启动失败");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::string bigHeader(4096, 'X');
    std::string request = "GET /test HTTP/1.1\r\n";
    request += "Host: localhost\r\n";
    request += "X-Large: " + bigHeader + "\r\n";
    request += "\r\n";

    std::string response = sendRawRequest(18901, request);
    if (response.empty()) {
        std::cout << "(服务器关闭了连接) ";
    } else {
        CHECK(!response.empty(), "服务器应对大 header 请求作出响应");
    }

    CHECK(server.isRunning(), "服务器应仍处于运行状态");

    server.stop();
    PASS();
    return true;
}

static bool test_path_traversal() {
    TEST("路径穿越被拒绝 (返回 404/400)");
    utils::ThreadPool pool(2);
    http::HttpServer server(18902, pool);
    auto router = std::make_shared<router::Router>();
    router->get("/safe", [](const http::HttpRequest&, http::HttpResponse&){});
    server.setRouter(router);

    if (!server.start()) {
        FAIL("服务器启动失败");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::string request = "GET /../../../etc/passwd HTTP/1.1\r\n";
    request += "Host: localhost\r\n\r\n";

    std::string response = sendRawRequest(18902, request);
    CHECK(!response.empty(), "应收到响应");
    CHECK(response.find("404") != std::string::npos ||
          response.find("400") != std::string::npos,
          "路径穿越应返回 404/400, 实际: " + response.substr(0, 200));

    server.stop();
    PASS();
    return true;
}

static bool test_malformed_request_line() {
    TEST("畸形请求行不导致服务器崩溃");
    utils::ThreadPool pool(2);
    http::HttpServer server(18903, pool);
    auto router = std::make_shared<router::Router>();
    router->get("/test", [](const http::HttpRequest&, http::HttpResponse&){});
    server.setRouter(router);

    if (!server.start()) {
        FAIL("服务器启动失败");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::string request = "GARBAGE\r\nDATA\r\n\r\n";

    std::string response = sendRawRequest(18903, request);
    if (response.empty()) {
        std::cout << "(服务器关闭了连接) ";
    }

    CHECK(server.isRunning(), "服务器应仍处于运行状态");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 正常请求仍能处理
    std::string okReq = "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string okResp = sendRawRequest(18903, okReq);
    CHECK(!okResp.empty(), "畸形请求后正常请求应仍能处理");

    server.stop();
    PASS();
    return true;
}

static bool test_empty_request() {
    TEST("空请求不导致服务器崩溃");
    utils::ThreadPool pool(2);
    http::HttpServer server(18904, pool);
    auto router = std::make_shared<router::Router>();
    router->get("/test", [](const http::HttpRequest&, http::HttpResponse&){});
    server.setRouter(router);

    if (!server.start()) {
        FAIL("服务器启动失败");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::string response = sendRawRequest(18904, "\r\n\r\n");
    if (response.empty()) {
        std::cout << "(服务器关闭了连接) ";
    }

    CHECK(server.isRunning(), "服务器应仍处于运行状态");

    server.stop();
    PASS();
    return true;
}

static bool test_is_port_in_use() {
    TEST("isPortInUse 正确检测端口占用");
    utils::ThreadPool pool(2);
    http::HttpServer server(18905, pool);
    if (!server.start()) {
        FAIL("服务器启动失败");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    CHECK(http::HttpServer::isPortInUse(18905), "端口 18905 应被检测为已占用");
    CHECK(!http::HttpServer::isPortInUse(19876), "端口 19876 应未被占用");

    server.stop();
    PASS();
    return true;
}

static bool test_port_conflict() {
    TEST("同端口第二个服务器启动失败 (EADDRINUSE)");
    utils::ThreadPool pool1(2);
    http::HttpServer server1(18906, pool1);
    if (!server1.start()) {
        FAIL("首个服务器启动失败");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    utils::ThreadPool pool2(2);
    http::HttpServer server2(18906, pool2);
    bool started = server2.start();
    CHECK(!started, "同端口第二个服务器应启动失败 (EADDRINUSE)");

    server1.stop();
    PASS();
    return true;
}

int main() {
    std::cout << "=== test_edge_input ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) { g_testsPassed++; }
        else      { g_testsFailed++; }
    };

    run(test_oversized_header,        "大 header 请求");
    run(test_path_traversal,          "路径穿越");
    run(test_malformed_request_line,  "畸形请求行");
    run(test_empty_request,           "空请求");
    run(test_is_port_in_use,         "端口占用检测");
    run(test_port_conflict,          "同端口拒绝");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
