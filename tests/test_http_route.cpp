// test_http_route.cpp — Router 路由匹配单元测试
// 验证：静态路由精确匹配、动态路由参数提取、
//       通配符匹配、404 未匹配、HTTP 方法区分、查询参数

#include "router/Router.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include <iostream>
#include <set>
#include <string>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)
// SKIP：报告为"跳过"（计入 g_testsSkipped），不计入通过
#define SKIP(msg) do { std::cout << "跳过 (" << msg << ")" << std::endl; ++g_testsSkipped; return true; } while(0)

static int g_testsPassed = 0;
static int g_testsFailed = 0;
static int g_testsSkipped = 0;

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

// 匹配语义的回归（哈希/分段快路径 vs 逐条 regex_match）。
//
// 优先级**与旧实现一致**：按注册顺序取第一条命中。哈希静态表只是"候选之一"，
// 静态命中不会因为它是静态就抢先 —— 若某条动态路由注册在它之前且同样命中，
// 动态路由先赢（见第一条断言：/users/:id 注册在前，因此 /users/me 命中的是
// 动态路由、id=me）。这正是旧实现（be00e05）的行为，本用例在旧实现上也通过。
//
// 除优先级之外，分段匹配严格复刻 pathToRegex 语义：含 ':' 的复杂形态
// （":name.json" / "b:c" / ":a:b"）整条退回正则，通配 '*' 与正则元字符同样走
// 正则 —— 这些钉在 test_param_syntax_equivalence 里。
static bool test_match_index_semantics() {
    TEST("哈希/分段匹配语义（注册顺序优先 + 复杂 :param 形态退回正则）");
    router::Router router;

    std::string hit;
    auto mk = [&hit](const char* tag) {
        return [&hit, tag](const http::HttpRequest& req, http::HttpResponse&) {
            hit = std::string(tag);
            auto it = req.getParams().find("id");
            if (it != req.getParams().end()) hit += ":" + it->second;
        };
    };

    // 注册顺序刻意让"动态路由在前、静态路由在后"
    router.get("/users/:id", mk("dynamic"));
    router.get("/users/me", mk("static"));
    router.get("/a/:x/b/:y", mk("two"));

    http::HttpRequest r1; r1.parse(makeRequest("GET", "/users/me"));
    http::HttpResponse s1; router.handleRequest(r1, s1);
    CHECK(hit == "dynamic:me", "注册在前的动态路由应先命中, 实际 '" << hit << "'");

    hit.clear();
    http::HttpRequest r2; r2.parse(makeRequest("GET", "/users/42"));
    http::HttpResponse s2; router.handleRequest(r2, s2);
    CHECK(hit == "dynamic:42", "动态路由应命中并提取参数, 实际 '" << hit << "'");

    hit.clear();
    http::HttpRequest r3; r3.parse(makeRequest("GET", "/a/1/b/2"));
    http::HttpResponse s3; router.handleRequest(r3, s3);
    CHECK(hit == "two", "两段参数路由应命中, 实际 '" << hit << "'");

    // 段数不同不命中（旧正则 ^/a/([^/]+)/b/([^/]+)$ 亦不命中）
    hit.clear();
    http::HttpRequest r4; r4.parse(makeRequest("GET", "/a/1/b/2/c"));
    http::HttpResponse s4; router.handleRequest(r4, s4);
    CHECK(hit.empty(), "段数不同不应命中, 实际 '" << hit << "'");
    CHECK(s4.getStatusCode() == 404, "应为 404, 实际 " << s4.getStatusCode());

    // 空段 :param 不匹配（([^/]+) 语义）
    hit.clear();
    http::HttpRequest r5; r5.parse(makeRequest("GET", "/users//me"));
    http::HttpResponse s5; router.handleRequest(r5, s5);
    CHECK(hit != "dynamic", "空段不应被 :param 吞掉, 实际 '" << hit << "'");

    // 重复注册同一静态路径：先注册者优先（与线性扫描一致）
    router::Router dup;
    std::string which;
    dup.get("/same", [&](const http::HttpRequest&, http::HttpResponse&) { which = "first"; });
    dup.get("/same", [&](const http::HttpRequest&, http::HttpResponse&) { which = "second"; });
    http::HttpRequest r6; r6.parse(makeRequest("GET", "/same"));
    http::HttpResponse s6; dup.handleRequest(r6, s6);
    CHECK(which == "first", "重复注册应命中第一条, 实际 '" << which << "'");

    // HEAD 回退到 GET（通过 methodIndex 的 GET 视图）
    router::Router headRouter;
    bool getCalled = false;
    headRouter.get("/h", [&](const http::HttpRequest&, http::HttpResponse&) { getCalled = true; });
    http::HttpRequest r7; r7.parse(makeRequest("HEAD", "/h"));
    http::HttpResponse s7; headRouter.handleRequest(r7, s7);
    CHECK(getCalled, "HEAD 应回退到 GET handler");
    CHECK(s7.getStatusCode() == 200, "HEAD 回退应得到 200, 实际 " << s7.getStatusCode());

    PASS();
    return true;
}

// ── 分段匹配与 pathToRegex 的严格等价（四个曾实测分叉的反例）──────────────
//
// 背景：分段快路径曾把"段首是 ':'"一律当成动态段，与 pathToRegex
// （正则 `:([a-zA-Z_][a-zA-Z0-9_]*)` 在整条路径上搜索、与段边界无关）
// 在四种形态上分叉：
//   A "/f/:name.json" + "/f/abc.json" → 后缀被参数吞掉（name=abc.json）
//   B "/f/:name.json" + "/f/abc"      → 旧实现 404，新实现被误命中
//   C "/a/b:c"        + "/a/bXYZ"     → 旧实现命中 c=XYZ，新实现 404
//   D "/x/:a:b"       + "/x/12"       → 旧实现 a=1,b=2，新实现只剩 a=12
// （另有"静态段里的正则元字符"与"通配 *"两类：'.' 必须按字面量匹配、'*' 必须
//  匹配任意子路径，二者都不能走整串全等。）
// 修复口径：只有"每一段都是纯静态段或纯 :标识符段"时才用分段快路径，
// 其余形态整条路由退回 pathRegex —— 因此上面每一条都必须与 be00e05 一致。
static bool test_param_syntax_equivalence() {
    TEST("复杂 :param 形态与 pathToRegex 等价（4 个反例 + 元字符/通配）");

    struct Case {
        const char* name;
        const char* pattern;
        const char* request;
        bool expectHit;
        const char* expectPairs;   // 期望参数，形如 "name=abc" 或 "a=1,b=2"；空 = 无参数
    };
    const Case cases[] = {
        {"A :param 带静态后缀，请求命中且参数不含后缀", "/f/:name.json", "/f/abc.json", true,  "name=abc"},
        {"B :param 带静态后缀，缺后缀不得命中",         "/f/:name.json", "/f/abc",      false, ""},
        {"C 段中部 ':'（字面量锚点）",                  "/a/b:c",        "/a/bXYZ",     true,  "c=XYZ"},
        {"D 同段两个 ':param'（旧实现只提交第一个）",   "/x/:a:b",       "/x/12",       true,  "a=1,b=2"},
        {"E 静态段里的 '.' 必须按字面量匹配",           "/lit/a.b",      "/lit/axb",    false, ""},
        {"F 静态段里的 '.' 命中字面量",                 "/lit/a.b",      "/lit/a.b",    true,  ""},
        {"G 通配 '*' 匹配任意子路径",                   "/files/*",      "/files/a/b",  true,  ""},
        {"H 通配 '*' 与静态段混合",                     "/files/*/meta", "/files/a/meta", true, ""},
        {"I 纯 :param 基线",                            "/u/:id",        "/u/42",       true,  "id=42"},
        {"J 空段不被 :param 吞掉",                      "/u/:id",        "/u/",         false, ""},
    };

    const auto splitPairs = [](const std::string& s) {
        std::set<std::string> out;
        std::size_t start = 0;
        while (start <= s.size() && !s.empty()) {
            std::size_t pos = s.find(',', start);
            std::string tok = s.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
            if (!tok.empty()) out.insert(tok);
            if (pos == std::string::npos) break;
            start = pos + 1;
        }
        return out;
    };

    for (const Case& c : cases) {
        router::Router rt;
        bool hit = false;
        std::set<std::string> gotPairs;
        rt.get(c.pattern, [&hit, &gotPairs](const http::HttpRequest& req, http::HttpResponse&) {
            hit = true;
            for (const auto& kv : req.getParams()) gotPairs.insert(kv.first + "=" + kv.second);
        });
        http::HttpRequest r; r.parse(makeRequest("GET", c.request));
        http::HttpResponse s; rt.handleRequest(r, s);

        CHECK(hit == c.expectHit, c.name << ": 命中判定不符（hit=" << hit
                                 << " status=" << s.getStatusCode() << "）");
        const std::set<std::string> want = splitPairs(c.expectPairs);
        if (gotPairs != want) {
            std::string detail = "期望 {";
            for (const auto& p : want) detail += p + ",";
            detail += "} 实际 {";
            for (const auto& p : gotPairs) detail += p + ",";
            detail += "}";
            FAIL(c.name << ": 参数不符（" << detail << "）");
        }
    }

    // 注册顺序优先的另一个方向：静态命中注册在前 → 静态赢（哈希快路径生效）
    {
        router::Router rt;
        std::string got;
        rt.get("/users/me", [&got](const http::HttpRequest&, http::HttpResponse&) { got = "static"; });
        rt.get("/users/:id", [&got](const http::HttpRequest& req, http::HttpResponse&) {
            got = "dynamic";
            auto it = req.getParams().find("id");
            if (it != req.getParams().end()) got += ":" + it->second;
        });
        http::HttpRequest r; r.parse(makeRequest("GET", "/users/me"));
        http::HttpResponse s; rt.handleRequest(r, s);
        CHECK(got == "static", "静态命中注册在前时应赢, 实际 '" << got << "'");

        // 同一张表里非静态的请求仍走动态路由
        got.clear();
        http::HttpRequest r2; r2.parse(makeRequest("GET", "/users/42"));
        http::HttpResponse s2; rt.handleRequest(r2, s2);
        CHECK(got == "dynamic:42", "非静态请求应由动态路由处理, 实际 '" << got << "'");
    }

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
    run(test_match_index_semantics,        "匹配索引语义");
    run(test_param_syntax_equivalence,     "复杂 :param 形态等价性");
    run(test_404_not_found,               "404 未找到");
    run(test_method_based_routing,         "方法分发");
    run(test_query_params,                 "查询参数");
    run(test_request_body,                 "请求体解析");
    run(test_not_found_custom_handler,     "自定义 404");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败, "
              << g_testsSkipped << " 跳过" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
