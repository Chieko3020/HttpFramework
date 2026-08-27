// test_http_session.cpp — Session 会话管理单元测试
// 验证：SessionManager 创建→读取→过期→清理、
//       SessionMiddleware Cookie 自动管理、
//       Session 数据操作 (set/get/has/remove/clear)

#include "session/SessionManager.h"
#include "session/Session.h"
#include "middleware/SessionMiddleware.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include <iostream>
#include <chrono>
#include <thread>

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

int main() {
    std::cout << "=== test_http_session ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) { g_testsPassed++; }
        else      { g_testsFailed++; }
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

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
