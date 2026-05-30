// test_http_route.cpp — Router 路由匹配单元测试
// 验证：静态路由精确匹配、动态路由参数提取、
//       通配符匹配、404 未匹配、HTTP 方法区分、查询参数

#include "router/Router.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include <iostream>
#include <string>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)

static int g_testsPassed = 0;
static int g_testsFailed = 0;

// 辅助：构造简单 HTTP 请求字符串
static std::string makeRequest(const std::string& method, const std::string& path,
                                const std::string& body = "") {
    std::string req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: localhost\r\n";
    if (!body.empty()) {
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    return req;
}

static bool test_static_route_exact_match() {
    TEST("静态路由 /api/status 精确匹配");
    router::Router router;

    bool called = false;
    router.get("/api/status", [&called](const http::HttpRequest&, http::HttpResponse&) {
        called = true;
    });

    http::HttpRequest req;
    req.parse(makeRequest("GET", "/api/status"));
    http::HttpResponse res;

    bool handled = router.handleRequest(req, res);
    CHECK(handled, "路由应被处理");
    CHECK(called, "handler 应被调用");
    CHECK(res.getStatusCode() == 200, "默认状态码应为 200");

    PASS();
    return true;
}

static bool test_dynamic_route_param_extraction() {
    TEST("动态路由 /users/:id 参数提取");
    router::Router router;

    std::string capturedId;
    router.get("/users/:id", [&capturedId](const http::HttpRequest& req, http::HttpResponse&) {
        capturedId = req.getParam("id");
    });

    http::HttpRequest req;
    req.parse(makeRequest("GET", "/users/42"));
    http::HttpResponse res;
    router.handleRequest(req, res);
    CHECK(capturedId == "42", "期望参数 id='42', 实际 '" << capturedId << "'");

    // 测试另一个 id
    http::HttpRequest req2;
    req2.parse(makeRequest("GET", "/users/alice"));
    http::HttpResponse res2;
    router.handleRequest(req2, res2);
    CHECK(capturedId == "alice", "期望参数 id='alice', 实际 '" << capturedId << "'");

    PASS();
    return true;
}

static bool test_multiple_dynamic_params() {
    TEST("路由 /posts/:year/:month 提取多个参数");
    router::Router router;

    std::string year, month;
    router.get("/posts/:year/:month", [&](const http::HttpRequest& req, http::HttpResponse&) {
        year = req.getParam("year");
        month = req.getParam("month");
    });

    http::HttpRequest req;
    req.parse(makeRequest("GET", "/posts/2024/03"));
    http::HttpResponse res;
    router.handleRequest(req, res);

    CHECK(year == "2024", "期望 year='2024', 实际 '" << year << "'");
    CHECK(month == "03", "期望 month='03', 实际 '" << month << "'");

    PASS();
    return true;
}

static bool test_wildcard_route() {
    TEST("通配符 * 匹配任意子路径");
    router::Router router;

    bool called = false;
    router.get("/files/*", [&](const http::HttpRequest&, http::HttpResponse&) {
        called = true;
    });

    // 嵌套路径
    http::HttpRequest req1;
    req1.parse(makeRequest("GET", "/files/images/photo.jpg"));
    http::HttpResponse res1;
    router.handleRequest(req1, res1);
    CHECK(called, "嵌套路径 handler 应被调用");
    called = false;

    // 根路径
    http::HttpRequest req2;
    req2.parse(makeRequest("GET", "/files/"));
    http::HttpResponse res2;
    router.handleRequest(req2, res2);
    CHECK(called, "通配符根路径 handler 应被调用");
    called = false;

    // /files 精确路径不匹配 /files/* 通配符
    http::HttpRequest req3;
    req3.parse(makeRequest("GET", "/files"));
    http::HttpResponse res3;
    router.handleRequest(req3, res3);
    CHECK(!called, "/files/* 不应匹配精确路径 /files");

    PASS();
    return true;
}

static bool test_404_not_found() {
    TEST("未注册路径返回 404");
    router::Router router;

    router.get("/hello", [](const http::HttpRequest&, http::HttpResponse&) {});

    http::HttpRequest req;
    req.parse(makeRequest("GET", "/nonexistent"));
    http::HttpResponse res;

    bool handled = router.handleRequest(req, res);
    CHECK(handled, "handleRequest 应返回 true (404 handler 被调用)");
    CHECK(res.getStatusCode() == 404, "期望 404, 实际 " << res.getStatusCode());

    PASS();
    return true;
}

static bool test_method_based_routing() {
    TEST("不同 HTTP 方法分发到不同 handler");
    router::Router router;

    bool getCalled = false, postCalled = false;

    router.get("/api/data", [&](const http::HttpRequest&, http::HttpResponse&) {
        getCalled = true;
    });
    router.post("/api/data", [&](const http::HttpRequest&, http::HttpResponse&) {
        postCalled = true;
    });

    http::HttpRequest getReq;
    getReq.parse(makeRequest("GET", "/api/data"));
    http::HttpResponse getRes;
    router.handleRequest(getReq, getRes);
    CHECK(getCalled, "GET handler 应被调用");
    CHECK(!postCalled, "POST handler 不应被调用");

    http::HttpRequest postReq;
    postReq.parse(makeRequest("POST", "/api/data", "hello"));
    http::HttpResponse postRes;
    router.handleRequest(postReq, postRes);
    CHECK(postCalled, "POST handler 应被调用");

    PASS();
    return true;
}

static bool test_query_params() {
    TEST("URL 查询参数被正确解析");
    http::HttpRequest req;
    req.parse(makeRequest("GET", "/search?q=hello&page=1"));

    CHECK(req.getQuery("q") == "hello", "期望 q=hello");
    CHECK(req.getQuery("page") == "1", "期望 page=1");

    PASS();
    return true;
}

static bool test_request_body() {
    TEST("POST 请求体被正确解析");
    http::HttpRequest req;
    req.parse(makeRequest("POST", "/submit", "name=Alice&age=30"));

    CHECK(req.getBody() == "name=Alice&age=30", "请求体不匹配");
    CHECK(req.getMethodString() == "POST", "方法应为 POST");

    PASS();
    return true;
}

static bool test_not_found_custom_handler() {
    TEST("自定义 404 handler 被调用");
    router::Router router;

    bool customNotFoundCalled = false;
    router.setNotFoundHandler([&](const http::HttpRequest&, http::HttpResponse& res) {
        customNotFoundCalled = true;
        res.setStatus(http::HttpStatus::NOT_FOUND);
        res.setJson("{\"error\":\"自定义 404\"}");
    });

    http::HttpRequest req;
    req.parse(makeRequest("GET", "/no-such-route"));
    http::HttpResponse res;
    router.handleRequest(req, res);

    CHECK(customNotFoundCalled, "自定义 404 handler 应被调用");
    CHECK(res.getStatusCode() == 404, "状态码应为 404");
    CHECK(res.getBody().find("自定义 404") != std::string::npos, "响应体应包含自定义消息");

    PASS();
    return true;
}

int main() {
    std::cout << "=== test_http_route ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) { g_testsPassed++; }
        else      { g_testsFailed++; }
    };

    run(test_static_route_exact_match,     "静态路由精确匹配");
    run(test_dynamic_route_param_extraction, "动态路由参数提取");
    run(test_multiple_dynamic_params,      "多个动态参数");
    run(test_wildcard_route,               "通配符路由");
    run(test_404_not_found,               "404 未找到");
    run(test_method_based_routing,         "方法分发");
    run(test_query_params,                 "查询参数");
    run(test_request_body,                 "请求体解析");
    run(test_not_found_custom_handler,     "自定义 404");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
