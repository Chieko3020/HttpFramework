// test_http_db.cpp — DbConnectionPool 数据库连接池单元测试
// 验证：连接池初始化失败优雅降级、连接获取/归还、SQL 查询、可用连接计数

#include <iostream>
#include <string>

#ifdef ENABLE_DATABASE
#include "utils/db/DbConnectionPool.h"
#include "utils/db/DbConnection.h"
#include "utils/db/DbException.h"
#endif

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)
#define SKIP(msg) do { std::cout << "跳过 (" << msg << ")" << std::endl; return true; } while(0)

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#ifdef ENABLE_DATABASE

static bool test_pool_creation() {
    TEST("连接池构造并初始化 (MySQL不可用时优雅降级)");
    db::DbConnectionPool pool("127.0.0.1", "test", "test", "testdb", 3306, 2);

    bool initialized = pool.initialize();

    if (!initialized) {
        SKIP("MySQL 不可用 — initialize() 返回 false (符合预期)");
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
    CHECK(!initialized, "无效凭据时 initialize() 应返回 false");
    CHECK(pool.getTotalConnections() == 0, "初始化失败后连接数应为 0");

    pool.shutdown();
    PASS();
    return true;
}

static bool test_pool_get_connection() {
    TEST("getConnection 返回已连接的数据库连接");
    db::DbConnectionPool pool("127.0.0.1", "test", "test", "testdb", 3306, 2);
    bool initialized = pool.initialize();

    if (!initialized) {
        SKIP("MySQL 不可用, 跳过连接获取测试");
    }

    auto conn = pool.getConnection();
    CHECK(conn != nullptr, "getConnection 应返回非空");
    CHECK(conn->isConnected(), "连接应处于已连接状态");

    pool.returnConnection(conn);

    auto conn2 = pool.getConnection();
    CHECK(conn2 != nullptr, "第二次 getConnection 也应成功");
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
        SKIP("MySQL 不可用, 跳过查询测试");
    }

    auto conn = pool.getConnection();
    CHECK(conn != nullptr, "应能获取连接");

    try {
        auto result = conn->executeQuery("SELECT 1 AS test_col");
        CHECK(result != nullptr, "查询应返回 ResultSet");
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
        SKIP("MySQL 不可用, 跳过连接计数测试");
    }

    size_t total = pool.getTotalConnections();
    CHECK(total == 2, "期望 2 个总连接, 实际 " << total);

    auto conn = pool.getConnection();
    size_t available = pool.getAvailableConnections();
    CHECK(available == total - 1, "借出一个后可用数应减 1");

    pool.returnConnection(conn);
    CHECK(pool.getAvailableConnections() == total, "归还后可用数应恢复");

    pool.shutdown();
    PASS();
    return true;
}

#else  // !ENABLE_DATABASE

// 数据库未编译时的桩测试
static bool test_db_not_compiled() {
    TEST("数据库支持未编译 (ENABLE_DATABASE 未定义)");
    std::cout << "跳过 (未链接 MySQL Connector)" << std::endl;
    return true;
}

#endif  // ENABLE_DATABASE

int main() {
    std::cout << "=== test_http_db ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) { g_testsPassed++; }
        else      { g_testsFailed++; }
    };

#ifdef ENABLE_DATABASE
    run(test_pool_creation,            "连接池初始化");
    run(test_pool_graceful_degradation, "优雅降级");
    run(test_pool_get_connection,      "获取连接");
    run(test_pool_query,               "SQL 查询");
    run(test_pool_available_count,     "可用连接计数");
#else
    run(test_db_not_compiled, "数据库未编译");
#endif

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
