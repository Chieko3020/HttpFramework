// test_http_db.cpp — DbConnectionPool 数据库连接池单元测试
// 验证：连接池初始化失败优雅降级、连接获取/归还、SQL 查询、可用连接计数
//
// 本机现实：多数开发机/CI 上**没有 MySQL server**（3306 未监听），但
// Connector/C++ 头文件可能装了 —— 于是 ENABLE_DATABASE 编译进来、用例却
// 只能跑失败路径。处理方式：
//   * 用例开始时探测 127.0.0.1:3306（与 DbConnectionPool 内部同一判据），
//     没有 server 就 SKIP 并打印明确原因，SKIP 计入 g_testsSkipped 而不是
//     g_testsPassed（退出码仍为 0，但报告里可区分）；
//   * **排除"因错误的原因通过"**：优雅降级用例断言 initialize() 返回 false 的
//     同时，必须断言 lastInitError() 给出的是"服务端不可达"；服务端可达时则
//     必须指向"凭据/库被拒"（rejected），不允许出现"任何 false 都算过"。

#include <iostream>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef ENABLE_DATABASE
#include "utils/db/DbConnectionPool.h"
#include "utils/db/DbConnection.h"
#include "utils/db/DbException.h"
#endif

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

#ifdef ENABLE_DATABASE

// 探测 127.0.0.1:3306 是否可连（与 DbConnectionPool 内部判据一致）
static bool mysqlServerReachable() {
    db::DbConnectionPool probe("127.0.0.1", "probe", "probe", "probe",
                               3306, /*maxConnections=*/1);
    probe.initialize();               // 结果无所谓，只为更新可达性
    bool reachable = probe.serverReachable();
    probe.shutdown();
    return reachable;
}

static bool test_pool_creation() {
    TEST("连接池构造并初始化 (MySQL不可用时优雅降级)");
    db::DbConnectionPool pool("127.0.0.1", "test", "test", "testdb", 3306, 2);

    bool initialized = pool.initialize();

    if (!initialized) {
        // 降级路径：必须能说清"为什么失败"，否则就是"因错误的原因通过"
        const std::string why = pool.lastInitError();
        std::cout << "(失败原因: " << why << ") ";
        CHECK(!why.empty(), "初始化失败必须给出 lastInitError()，不能是空原因");

        if (!pool.serverReachable()) {
            // 本机没有 server：原因必须是"服务端不可达"。若 lastInitError 只给出
            // 笼统的失败字样（无法区分不可达/被拒），这一条就会失败。
            CHECK(why.find("unreachable") != std::string::npos,
                  "无 server 时失败原因应指出服务端不可达, 实际: " << why);
            SKIP(std::string("MySQL server 不可达 (127.0.0.1:3306) — 原因已记录: ") + why);
        }
        // 服务端可达却仍然初始化失败：这是"凭据/库被拒"的合法降级路径，
        // 原因必须指向连接被拒，而不是其它异常
        CHECK(why.find("rejected") != std::string::npos,
              "服务端可达时失败原因应指出凭据/库被拒, 实际: " << why);
        PASS();
        return true;
    }

    CHECK(pool.getTotalConnections() > 0, "成功初始化后应有连接");
    pool.shutdown();
    PASS();
    return true;
}

static bool test_pool_graceful_degradation() {
    TEST("无效凭据时连接池优雅降级");
    db::DbConnectionPool pool("127.0.0.1", "invalid_user", "invalid_pass",
                               "nonexistent_db", 3306, 1);

    bool initialized = pool.initialize();
    const std::string why = pool.lastInitError();
    std::cout << "(server_reachable=" << (pool.serverReachable() ? 1 : 0)
              << " why=" << why << ") ";

    CHECK(!initialized, "无效凭据时 initialize() 应返回 false");
    CHECK(pool.getTotalConnections() == 0, "初始化失败后连接数应为 0");
    CHECK(!why.empty(), "失败必须带原因（不能与'服务端未启动'混为一谈）");

    if (!pool.serverReachable()) {
        // 本机没有 server：只能验证"降级 + 原因可读"，不能声称验证了凭据校验
        pool.shutdown();
        SKIP("无 MySQL server，无法真正验证凭据校验（已断言降级与原因可读）");
    }
    CHECK(why.find("rejected") != std::string::npos,
          "服务端可达时原因应指出凭据/库被拒, 实际: " << why);

    pool.shutdown();
    PASS();
    return true;
}

static bool test_pool_get_connection() {
    TEST("getConnection 返回已连接的数据库连接");
    db::DbConnectionPool pool("127.0.0.1", "test", "test", "testdb", 3306, 2);
    bool initialized = pool.initialize();

    if (!initialized) {
        SKIP(std::string("MySQL 不可用（") + pool.lastInitError() + "）");
    }

    auto conn = pool.getConnection();
    CHECK(conn != nullptr, "getConnection 应返回非空");
    CHECK(conn->isConnected(), "连接应处于已连接状态");

    pool.returnConnection(conn);

    auto conn2 = pool.getConnection();
    CHECK(conn2 != nullptr, "第二次 getConnection 也应成功");
    CHECK(conn2->isConnected(), "第二次拿到的连接也应处于已连接状态");
    pool.returnConnection(conn2);

    pool.shutdown();
    PASS();
    return true;
}

static bool test_pool_query() {
    TEST("通过连接池执行 SQL 查询");
    db::DbConnectionPool pool("127.0.0.1", "test", "test", "testdb", 3306, 2);
    bool initialized = pool.initialize();

    if (!initialized) {
        SKIP(std::string("MySQL 不可用（") + pool.lastInitError() + "）");
    }

    auto conn = pool.getConnection();
    CHECK(conn != nullptr, "应能获取连接");

    try {
        auto result = conn->executeQuery("SELECT 1 AS test_col");
        CHECK(result != nullptr, "查询应返回 ResultSet");
        // 真正取一行：只断言非空会漏掉"结果集是空的"这类失败
        CHECK(result->next(), "SELECT 1 应至少返回一行");
        CHECK(result->getInt("test_col") == 1, "test_col 应为 1");
    } catch (const db::DbException& e) {
        FAIL(std::string("查询抛出异常: ") + e.what());
    }

    pool.returnConnection(conn);
    pool.shutdown();
    PASS();
    return true;
}

static bool test_pool_available_count() {
    TEST("getAvailableConnections 反映真实可用连接数");
    db::DbConnectionPool pool("127.0.0.1", "test", "test", "testdb", 3306, 2);
    bool initialized = pool.initialize();

    if (!initialized) {
        SKIP(std::string("MySQL 不可用（") + pool.lastInitError() + "）");
    }

    size_t total = pool.getTotalConnections();
    CHECK(total == 2, "期望 2 个总连接, 实际 " << total);

    auto conn = pool.getConnection();
    size_t available = pool.getAvailableConnections();
    CHECK(available == total - 1, "借出一个后可用数应减 1");
    CHECK(pool.getTotalConnections() == total, "借出不应改变总连接数");

    pool.returnConnection(conn);
    CHECK(pool.getAvailableConnections() == total, "归还后可用数应恢复");

    pool.shutdown();
    PASS();
    return true;
}

// T1：probeServer 必须认识主机名与 IPv6 —— 原来只认 IPv4 字面量，
// host="localhost"/"::1" 一律被判"不可达"，lastInitError 误报 "TCP connect failed"。
// 本用例不依赖 MySQL server：直接对一个刚建好的本地监听端口做探测。
static bool test_probe_server_hostname() {
    TEST("TCP 探测识别主机名/IPv6（不再把 localhost 判成不可达）");

    int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(ls >= 0, "创建监听 socket 失败");
    int on = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;                       // 内核选端口
    CHECK(::bind(ls, (struct sockaddr*)&a, sizeof(a)) == 0, "bind 失败");
    CHECK(::listen(ls, 8) == 0, "listen 失败");
    socklen_t alen = sizeof(a);
    CHECK(::getsockname(ls, (struct sockaddr*)&a, &alen) == 0, "getsockname 失败");
    const int port = ntohs(a.sin_port);

    using PR = db::DbConnectionPool::ProbeResult;
    const PR ipv4 = db::DbConnectionPool::probeServer("127.0.0.1", port);
    const PR name = db::DbConnectionPool::probeServer("localhost", port);
    const PR bogus = db::DbConnectionPool::probeServer("no-such-host.invalid", port);

    std::cout << "(127.0.0.1=" << static_cast<int>(ipv4)
              << " localhost=" << static_cast<int>(name)
              << " bogus=" << static_cast<int>(bogus) << ") ";

    CHECK(ipv4 == PR::Available, "127.0.0.1 应判为可连");
    CHECK(name == PR::Available,
          "localhost 应判为可连（修复前 inet_pton(AF_INET,\"localhost\") 失败 ⇒ 误判不可达）");
    CHECK(bogus == PR::Unresolved,
          "不可解析的主机名应判为 Unresolved（而不是伪装成 TCP connect failed）");

    // 端口无人监听：仍是 Refused（不是 Unresolved）
    const PR closed = db::DbConnectionPool::probeServer("127.0.0.1", port + 1);
    CHECK(closed != PR::Unresolved, "已解析地址的关闭端口不应判为 Unresolved");
    ::close(ls);
    PASS();
    return true;
}

#else  // !ENABLE_DATABASE

// 数据库未编译时的桩测试
static bool test_db_not_compiled() {
    SKIP("未链接 MySQL Connector (ENABLE_DATABASE 未定义)");
}

#endif  // ENABLE_DATABASE

int main() {
    std::cout << "=== test_http_db ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        g_skipFlag = false;   // fn() 里的 SKIP 会置位
        if (fn()) { if (!g_skipFlag) ++g_testsPassed; }
        else if (!g_skipFlag) { ++g_testsFailed; }
    };

#ifdef ENABLE_DATABASE
    std::cout << "[环境] MySQL server 127.0.0.1:3306: "
              << (mysqlServerReachable() ? "可达" : "不可达（相关用例将报告为跳过）")
              << std::endl;
    run(test_pool_creation,             "连接池初始化");
    run(test_pool_graceful_degradation, "优雅降级");
    run(test_pool_get_connection,       "获取连接");
    run(test_pool_query,                "SQL 查询");
    run(test_pool_available_count,      "可用连接计数");
    run(test_probe_server_hostname,     "TCP 探测主机名/IPv6");
#else
    run(test_db_not_compiled, "数据库未编译");
#endif

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败, "
              << g_testsSkipped << " 跳过（总用例 "
              << (g_testsPassed + g_testsFailed + g_testsSkipped) << "）" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
