// test_http_middleware.cpp — Middleware 中间件链单元测试
// 验证：next() 流转、路径过滤不误拦截、
//       鉴权中间件不调 next() 时请求短路

#include "router/Router.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include <iostream>
#include <vector>
#include <string>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)

static int g_testsPassed = 0;
static int g_testsFailed = 0;

static std::string makeRequest(const std::string& method, const std::string& path,
                                const std::string& extraHeaders = "",
                                const std::string& body = "") {
    std::string req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: localhost\r\n";
    req += extraHeaders;
    if (!body.empty()) {
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    return req;
}

static bool test_next_flow() {
    TEST("next() 在中间件链中正确流转 (洋葱模型)");
    router::Router router;

    std::vector<std::string> order;

    router.use([&](const http::HttpRequest&, http::HttpResponse&, std::function<void()> next) {
        order.push_back("mw1_前");
        next();
        order.push_back("mw1_后");
    });
    router.use([&](const http::HttpRequest&, http::HttpResponse&, std::function<void()> next) {
        order.push_back("mw2_前");
        next();
        order.push_back("mw2_后");
    });

    router.get("/test", [&](const http::HttpRequest&, http::HttpResponse&) {
        order.push_back("handler");
    });

    http::HttpRequest req;
    req.parse(makeRequest("GET", "/test"));
    http::HttpResponse res;
    router.handleRequest(req, res);

    // 预期顺序: mw1_前 → mw2_前 → handler → mw2_后 → mw1_后
    CHECK(order.size() == 5, "期望 5 步, 实际 " << order.size());
    CHECK(order[0] == "mw1_前", "第 0 步应为 mw1_前");
    CHECK(order[1] == "mw2_前", "第 1 步应为 mw2_前");
    CHECK(order[2] == "handler", "第 2 步应为 handler");
    CHECK(order[3] == "mw2_后", "第 3 步应为 mw2_后");
    CHECK(order[4] == "mw1_后", "第 4 步应为 mw1_后");

    PASS();
    return true;
}

static bool test_global_middleware_all_paths() {
    TEST("全局中间件对所有路径生效");
    router::Router router;

    int count = 0;
    router.use([&count](const http::HttpRequest&, http::HttpResponse&, std::function<void()> next) {
        count++;
        next();
    });

    router.get("/a", [](const http::HttpRequest&, http::HttpResponse&) {});
    router.get("/b", [](const http::HttpRequest&, http::HttpResponse&) {});

    http::HttpRequest req1; req1.parse(makeRequest("GET", "/a"));
    http::HttpResponse res1; router.handleRequest(req1, res1);
    CHECK(count == 1, "第一次请求后计数应为 1");

    http::HttpRequest req2; req2.parse(makeRequest("GET", "/b"));
    http::HttpResponse res2; router.handleRequest(req2, res2);
    CHECK(count == 2, "第二次请求后计数应为 2");

    // 未匹配路径也触发全局中间件
    http::HttpRequest req3; req3.parse(makeRequest("GET", "/c"));
    http::HttpResponse res3; router.handleRequest(req3, res3);
    CHECK(count == 3, "未匹配路径也触发全局中间件, 期望 3 实际 " << count);

    PASS();
    return true;
}

static bool test_path_filtered_middleware() {
    TEST("路径过滤中间件 /api/* 不拦截 /other");
    router::Router router;

    int apiMwCount = 0;
    int otherMwCount = 0;

    router.use("/api/*", [&apiMwCount](const http::HttpRequest&, http::HttpResponse&, std::function<void()> next) {
        apiMwCount++;
        next();
    });

    router.use("/other/*", [&otherMwCount](const http::HttpRequest&, http::HttpResponse&, std::function<void()> next) {
        otherMwCount++;
        next();
    });

    router.get("/api/status", [](const http::HttpRequest&, http::HttpResponse&) {});
    router.get("/other/status", [](const http::HttpRequest&, http::HttpResponse&) {});

    http::HttpRequest req1; req1.parse(makeRequest("GET", "/api/status"));
    http::HttpResponse res1; router.handleRequest(req1, res1);
    CHECK(apiMwCount == 1, "/api/status 应触发 api 中间件, 实际触发 " << apiMwCount << " 次");
    CHECK(otherMwCount == 0, "/api/status 不应触发 other 中间件");

    http::HttpRequest req2; req2.parse(makeRequest("GET", "/other/status"));
    http::HttpResponse res2; router.handleRequest(req2, res2);
    CHECK(apiMwCount == 1, "/other/status 不应触发 api 中间件");
    CHECK(otherMwCount == 1, "/other/status 应触发 other 中间件");

    PASS();
    return true;
}

static bool test_short_circuit_auth() {
    TEST("鉴权中间件不调 next() 时请求短路返回 401");
    router::Router router;

    bool handlerCalled = false;

    router.use([&](const http::HttpRequest& req, http::HttpResponse& res, std::function<void()> next) {
        if (req.getHeader("Authorization").empty()) {
            res.setStatus(http::HttpStatus::UNAUTHORIZED);
            res.setJson("{\"error\":\"未授权\"}");
            return;  // 不调 next()
        }
        next();
    });

    router.get("/protected", [&](const http::HttpRequest&, http::HttpResponse&) {
        handlerCalled = true;
    });

    http::HttpRequest req;
    req.parse(makeRequest("GET", "/protected"));
    http::HttpResponse res;
    router.handleRequest(req, res);

    CHECK(!handlerCalled, "鉴权失败时 handler 不应被调用");
    CHECK(res.getStatusCode() == 401, "期望 401, 实际 " << res.getStatusCode());
    CHECK(res.getBody().find("未授权") != std::string::npos, "响应体应包含错误信息");

    PASS();
    return true;
}

static bool test_auth_pass_through() {
    TEST("鉴权中间件携带合法 Token 时放行");
    router::Router router;

    bool handlerCalled = false;

    router.use([&](const http::HttpRequest& req, http::HttpResponse& res, std::function<void()> next) {
        if (req.getHeader("Authorization").empty()) {
            res.setStatus(http::HttpStatus::UNAUTHORIZED);
            res.setJson("{\"error\":\"未授权\"}");
            return;
        }
        next();
    });

    router.get("/protected", [&](const http::HttpRequest&, http::HttpResponse&) {
        handlerCalled = true;
    });

    http::HttpRequest req;
    req.parse(makeRequest("GET", "/protected", "Authorization: Bearer token123\r\n"));
    http::HttpResponse res;
    router.handleRequest(req, res);

    CHECK(handlerCalled, "合法 Token 时 handler 应被调用");
    CHECK(res.getStatusCode() == 200, "期望 200, 实际 " << res.getStatusCode());

    PASS();
    return true;
}

static bool test_multiple_scoped_middleware() {
    TEST("同一路径前缀上多个中间件按注册顺序执行");
    router::Router router;

    std::vector<int> called;

    router.use("/admin/*", [&](const http::HttpRequest&, http::HttpResponse&, std::function<void()> next) {
        called.push_back(1);
        next();
    });
    router.use("/admin/*", [&](const http::HttpRequest&, http::HttpResponse&, std::function<void()> next) {
        called.push_back(2);
        next();
    });

    router.get("/admin/dashboard", [&](const http::HttpRequest&, http::HttpResponse&) {
        called.push_back(3);
    });

    http::HttpRequest req;
    req.parse(makeRequest("GET", "/admin/dashboard"));
    http::HttpResponse res;
    router.handleRequest(req, res);

    CHECK(called.size() == 3, "期望 3 次调用, 实际 " << called.size());
    if (called.size() == 3) {
        CHECK(called[0] == 1 && called[1] == 2 && called[2] == 3, "执行顺序应为 1→2→3");
    }

    PASS();
    return true;
}

int main() {
    std::cout << "=== test_http_middleware ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) { g_testsPassed++; }
        else      { g_testsFailed++; }
    };

    run(test_next_flow,                   "next() 洋葱流转");
    run(test_global_middleware_all_paths,  "全局中间件");
    run(test_path_filtered_middleware,     "路径过滤中间件");
    run(test_short_circuit_auth,          "鉴权短路");
    run(test_auth_pass_through,           "鉴权放行");
    run(test_multiple_scoped_middleware,   "多中间件顺序");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
