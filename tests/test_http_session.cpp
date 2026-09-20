// test_http_session.cpp — Session 会话管理单元测试
// 验证：SessionManager 创建→读取→过期→清理、
//       SessionMiddleware Cookie 自动管理、
//       Session 数据操作 (set/get/has/remove/clear)

#include "session/SessionManager.h"
#include "session/Session.h"
#include "session/SessionStorage.h"
#include "middleware/SessionMiddleware.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include <filesystem>
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <thread>

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

// ── Session 基础操作 ──

static bool test_session_create_and_access() {
    TEST("Session 基本 CRUD 操作");
    auto session = std::make_shared<session::Session>("test-session-001");

    CHECK(session->getId() == "test-session-001", "id 不匹配");
    CHECK(session->isValid(), "新会话应有效");
    CHECK(!session->isExpired(), "新会话不应过期");

    session->set("username", "alice");
    session->set("role", "admin");
    CHECK(session->get("username") == "alice", "get username 失败");
    CHECK(session->get("role") == "admin", "get role 失败");
    CHECK(session->has("username"), "has username 应为 true");
    CHECK(!session->has("nonexistent"), "不存在的 key 应返回 false");

    // 不存在的 key 返回空字符串
    CHECK(session->get("nonexistent") == "", "不存在的 key 应返回空串");

    session->remove("role");
    CHECK(!session->has("role"), "remove 后 has 应为 false");

    session->set("temp", "value");
    session->clear();
    CHECK(!session->has("temp"), "clear 后 temp 应不存在");
    CHECK(!session->has("username"), "clear 后 username 应不存在");

    PASS();
    return true;
}

static bool test_session_expiration() {
    TEST("Session 过期检查");
    auto session = std::make_shared<session::Session>("exp-test");

    session->setExpirationTime(std::chrono::seconds(1));
    session->touch();

    CHECK(!session->isExpired(), "立即检查不应过期");
    CHECK(session->isValid(), "应处于有效状态");

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(session->isExpired(), "1.1 秒后应过期");
    CHECK(!session->isValid(), "过期后应无效");

    PASS();
    return true;
}

static bool test_session_touch() {
    TEST("touch 重置最后访问时间");
    auto session = std::make_shared<session::Session>("touch-test");
    session->setExpirationTime(std::chrono::seconds(2));
    session->touch();

    auto firstAccess = session->getLastAccessTime();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    session->touch();
    CHECK(!session->isExpired(), "touch 后不应过期");

    auto secondAccess = session->getLastAccessTime();
    CHECK(secondAccess > firstAccess, "touch 应更新最后访问时间");

    PASS();
    return true;
}

// ── SessionManager ──

static bool test_session_manager_create_get() {
    TEST("SessionManager 创建和获取会话");
    session::SessionManager mgr(std::chrono::seconds(60));

    auto s1 = mgr.createSession();
    CHECK(s1 != nullptr, "创建的会话不应为空");
    CHECK(!s1->getId().empty(), "会话 id 不应为空");

    auto s2 = mgr.getSession(s1->getId());
    CHECK(s2 != nullptr, "getSession 应返回会话");
    CHECK(s2->getId() == s1->getId(), "id 应一致");

    auto s3 = mgr.getSession("nonexistent-id");
    CHECK(s3 == nullptr, "不存在的 id 应返回 nullptr");

    PASS();
    return true;
}

static bool test_session_manager_remove() {
    TEST("SessionManager 删除会话");
    session::SessionManager mgr;

    auto s = mgr.createSession();
    std::string id = s->getId();

    CHECK(mgr.hasSession(id), "应存在该会话");
    CHECK(mgr.getSessionCount() == 1, "计数应为 1");

    mgr.removeSession(id);
    CHECK(!mgr.hasSession(id), "删除后不应存在");
    CHECK(mgr.getSessionCount() == 0, "删除后计数应为 0");

    PASS();
    return true;
}

static bool test_session_manager_create_with_id() {
    TEST("SessionManager 使用自定义 id 创建会话");
    session::SessionManager mgr;

    auto s = mgr.createSession("my-custom-id");
    CHECK(s->getId() == "my-custom-id", "自定义 id 应保留");
    CHECK(mgr.hasSession("my-custom-id"), "manager 应包含自定义 id");

    PASS();
    return true;
}

static bool test_session_manager_cleanup() {
    TEST("SessionManager 清理过期会话");
    session::SessionManager mgr(std::chrono::seconds(1));

    mgr.createSession();
    mgr.createSession();

    CHECK(mgr.getActiveSessionCount() == 2, "应有 2 个活跃会话");

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    mgr.cleanupExpiredSessions();
    CHECK(mgr.getActiveSessionCount() == 0, "过期会话应全部清理");

    PASS();
    return true;
}

static bool test_session_manager_get_all() {
    TEST("getAllSessions 只返回活跃会话");
    session::SessionManager mgr;

    mgr.createSession("s1");
    mgr.createSession("s2");
    mgr.createSession("s3");

    auto all = mgr.getAllSessions();
    CHECK(all.size() == 3, "期望 3 个会话, 实际 " << all.size());

    mgr.removeSession("s2");
    all = mgr.getAllSessions();
    CHECK(all.size() == 2, "删除后期望 2 个会话, 实际 " << all.size());

    PASS();
    return true;
}

static bool test_session_manager_set_default_expiration() {
    TEST("setDefaultExpiration 对新会话生效");
    session::SessionManager mgr(std::chrono::seconds(100));
    mgr.setDefaultExpiration(std::chrono::seconds(300));

    auto s = mgr.createSession();
    CHECK(s->getExpirationTime() == std::chrono::seconds(300),
          "新会话应使用新的默认过期时间");

    PASS();
    return true;
}

// ── SessionMiddleware ──

static bool test_session_middleware_creates_session() {
    TEST("SessionMiddleware 首次请求创建会话并设置 Cookie");
    auto mgr = std::make_shared<session::SessionManager>();

    middleware::SessionMiddleware mw(mgr);

    http::HttpRequest req;
    req.parse(makeRequest("GET", "/api/test"));
    http::HttpResponse res;

    bool nextCalled = false;
    auto next = [&nextCalled]() { nextCalled = true; };

    mw(req, res, next);

    CHECK(nextCalled, "next() 应被调用");
    CHECK(req.hasUserData("session"), "请求应包含 session 用户数据");

    std::string sessionId = req.getUserData("session");
    CHECK(!sessionId.empty(), "session id 不应为空");
    CHECK(mgr->hasSession(sessionId), "manager 应包含该会话");

    // 响应应包含 Set-Cookie 头
    std::string responseStr = res.toString();
    CHECK(responseStr.find("Set-Cookie") != std::string::npos,
          "响应应包含 Set-Cookie 头");

    PASS();
    return true;
}

static bool test_session_middleware_reuses_session() {
    TEST("SessionMiddleware 从 Cookie 复用已有会话");
    auto mgr = std::make_shared<session::SessionManager>();
    auto session = mgr->createSession();
    std::string existingId = session->getId();
    session->set("counter", "0");

    middleware::SessionMiddleware mw(mgr);

    http::HttpRequest req;
    req.parse(makeRequest("GET", "/api/test",
                          "Cookie: session_id=" + existingId + "\r\n"));
    http::HttpResponse res;

    bool nextCalled = false;
    mw(req, res, [&nextCalled]() { nextCalled = true; });

    CHECK(nextCalled, "next() 应被调用");
    CHECK(req.getUserData("session") == existingId, "应复用已有的 session id");

    auto reusedSession = mgr->getSession(existingId);
    CHECK(reusedSession != nullptr, "会话应仍存在");
    CHECK(reusedSession->get("counter") == "0", "会话数据应保留");

    PASS();
    return true;
}

// ── M12：文件存储的会话 ID 校验与原子写 ──
static bool test_file_storage_hardening() {
    TEST("M12 文件存储拒绝非法 sessionId（路径穿越）且写入可读回");
    const std::string dir = "/tmp/hf_session_m12";
    { int rc = system(("rm -rf " + dir).c_str()); (void)rc; }
    session::FileSessionStorage store(dir);

    // 路径穿越：非法 ID 不得落到存储目录之外
    const std::string evil = "../../etc/hf_pwned";
    auto s1 = std::make_shared<session::Session>(evil);
    s1->set("k", "v");
    CHECK(!store.saveSession(s1), "非法 sessionId 不得被写入");
    CHECK(!std::filesystem::exists("/etc/hf_pwned.session"), "不得写出存储目录之外的文件");
    CHECK(store.loadSession(evil) == nullptr, "非法 sessionId 读取应返回 nullptr");
    CHECK(!store.hasSession(evil), "非法 sessionId hasSession 应为 false");
    CHECK(!store.deleteSession(evil), "非法 sessionId 删除应返回 false");

    // 合法 ID：写→读→原子写留下的临时文件不应残留
    const std::string good = "abcdef0123456789abcdef0123456789";
    auto s2 = std::make_shared<session::Session>(good);
    s2->set("user", "chieko");
    CHECK(store.saveSession(s2), "合法会话应写入成功");
    auto back = store.loadSession(good);
    CHECK(back != nullptr, "应能读回会话");
    CHECK(back->get("user") == "chieko", "会话数据应完整");
    CHECK(!std::filesystem::exists(dir + "/" + good + ".session.tmp"), "不得残留临时文件");

    { int rc = system(("rm -rf " + dir).c_str()); (void)rc; }
    PASS();
    return true;
}

static bool test_session_cookie_refresh() {
    TEST("M12 已有会话的响应会刷新 Set-Cookie（滑动续期不脱钩）");
    auto mgr = std::make_shared<session::SessionManager>(std::chrono::seconds(60));
    auto mw = middleware::session_utils::createSessionMiddleware(mgr);

    // 第一次请求：新会话 → 下发 cookie
    http::HttpRequest req1;
    http::HttpResponse res1;
    bool next1 = false;
    mw(req1, res1, [&next1]() { next1 = true; });
    CHECK(next1, "中间件应调用 next()");
    const std::string cookie1 = res1.getHeader("Set-Cookie");
    CHECK(!cookie1.empty(), "新会话应下发 Set-Cookie");

    // 提取 sessionId 并放进第二个请求的 Cookie 头
    const std::string id = cookie1.substr(cookie1.find('=') + 1,
                                          cookie1.find(';') - cookie1.find('=') - 1);
    http::HttpRequest req2;
    req2.parse("GET / HTTP/1.1\r\nHost: x\r\nCookie: session_id=" + id + "\r\n\r\n");
    http::HttpResponse res2;
    bool next2 = false;
    mw(req2, res2, [&next2]() { next2 = true; });
    const std::string cookie2 = res2.getHeader("Set-Cookie");
    std::cout << "(第二次请求是否刷新 cookie=" << (cookie2.empty() ? "否" : "是") << ") ";
    CHECK(!cookie2.empty(), "已有会话也必须刷新 Set-Cookie（否则 cookie 先于会话过期）");
    CHECK(cookie2.find(id) != std::string::npos, "刷新时不得换掉 sessionId");

    PASS();
    return true;
}

int main() {
    std::cout << "=== test_http_session ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        g_skipFlag = false;   // fn() 里的 SKIP 会置位
        if (fn()) { if (!g_skipFlag) ++g_testsPassed; }
        else if (!g_skipFlag) { ++g_testsFailed; }
    };

    run(test_session_create_and_access,        "会话基本操作");
    run(test_session_expiration,               "会话过期");
    run(test_session_touch,                    "会话 touch");
    run(test_session_manager_create_get,       "管理器创建与获取");
    run(test_session_manager_remove,           "管理器删除");
    run(test_session_manager_create_with_id,   "管理器自定义 id");
    run(test_session_manager_cleanup,          "管理器清理过期");
    run(test_session_manager_get_all,          "管理器获取全部");
    run(test_session_manager_set_default_expiration, "设置默认过期");
    run(test_session_middleware_creates_session, "中间件创建会话");
    run(test_session_middleware_reuses_session,  "中间件复用会话");
    run(test_file_storage_hardening,             "M12 文件存储加固");
    run(test_session_cookie_refresh,             "M12 cookie 续期刷新");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败, "
              << g_testsSkipped << " 跳过（总用例 "
              << (g_testsPassed + g_testsFailed + g_testsSkipped) << "）" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
