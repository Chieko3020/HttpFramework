// test_infra_logger.cpp — Logger 单元测试
// 验证：setMinLevel(Info) 后 LOG_DEBUG 被丢弃、
//       LOG_INFO/WARN/ERROR 输出到 stderr、
//       initFileLog() 文件输出、多线程写入不交错

#include "utils/Logger.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <vector>
#include <sys/stat.h>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)

static int g_testsPassed = 0;
static int g_testsFailed = 0;

// 辅助：清理目录
static void cleanDir(const std::string& dir) {
    int ret = system(("rm -rf " + dir).c_str());
    (void)ret;
}

static bool test_level_filter_debug() {
    TEST("默认 minLevel=Info 时 LOG_DEBUG 被过滤");
    http::Logger::shutdownFileLog();

    std::string logDir = "/tmp/test_logger_debug";
    cleanDir(logDir);
    mkdir(logDir.c_str(), 0755);

    bool ok = http::Logger::initFileLog(logDir, "test.log");
    CHECK(ok, "initFileLog 失败");

    LOG_DEBUG("TestModule", "这条调试信息应被过滤掉");
    LOG_INFO("TestModule", "这条信息应出现在日志中");

    http::Logger::shutdownFileLog();

    std::ifstream logFile(logDir + "/test.log");
    CHECK(logFile.is_open(), "日志文件未找到");
    std::string content((std::istreambuf_iterator<char>(logFile)),
                         std::istreambuf_iterator<char>());

    CHECK(content.find("过滤掉") == std::string::npos,
          "DEBUG 消息不应出现在日志中");
    CHECK(content.find("应出现在日志中") != std::string::npos,
          "INFO 消息应出现在日志中");

    cleanDir(logDir);
    PASS();
    return true;
}

static bool test_level_filter_info_warn_error() {
    TEST("setMinLevel(Debug) 后四个级别全部输出");
    http::Logger::shutdownFileLog();
    http::Logger::setMinLevel(http::LogLevel::Debug);

    std::string logDir = "/tmp/test_logger_all";
    cleanDir(logDir);
    mkdir(logDir.c_str(), 0755);

    http::Logger::initFileLog(logDir, "test.log");

    LOG_DEBUG("T", "debug 消息 42");
    LOG_INFO("T",  "info 消息 43");
    LOG_WARN("T",  "warn 消息 44");
    LOG_ERROR("T", "error 消息 45");

    http::Logger::shutdownFileLog();

    std::ifstream logFile(logDir + "/test.log");
    std::string content((std::istreambuf_iterator<char>(logFile)),
                         std::istreambuf_iterator<char>());

    CHECK(content.find("debug 消息 42") != std::string::npos, "DEBUG 级别缺失");
    CHECK(content.find("info 消息 43")  != std::string::npos, "INFO 级别缺失");
    CHECK(content.find("warn 消息 44")  != std::string::npos, "WARN 级别缺失");
    CHECK(content.find("error 消息 45") != std::string::npos, "ERROR 级别缺失");

    http::Logger::setMinLevel(http::LogLevel::Info);
    cleanDir(logDir);
    PASS();
    return true;
}

static bool test_file_log_output() {
    TEST("initFileLog 写入正确的文件路径");
    http::Logger::shutdownFileLog();

    std::string logDir = "/tmp/test_logger_file";
    cleanDir(logDir);
    mkdir(logDir.c_str(), 0755);

    bool ok = http::Logger::initFileLog(logDir, "mylog.log");
    CHECK(ok, "initFileLog 应成功");

    LOG_INFO("TEST", "你好, 文件日志");

    http::Logger::shutdownFileLog();

    std::ifstream logFile(logDir + "/mylog.log");
    CHECK(logFile.is_open(), "日志文件应存在");
    std::string content((std::istreambuf_iterator<char>(logFile)),
                         std::istreambuf_iterator<char>());
    CHECK(content.find("你好, 文件日志") != std::string::npos, "消息未在文件中找到");

    cleanDir(logDir);
    PASS();
    return true;
}

static bool test_log_format() {
    TEST("日志输出包含级别标签和模块名");
    http::Logger::shutdownFileLog();

    std::string logDir = "/tmp/test_logger_format";
    cleanDir(logDir);
    mkdir(logDir.c_str(), 0755);

    http::Logger::initFileLog(logDir, "test.log");

    LOG_WARN("MyModule", "格式测试消息");

    http::Logger::shutdownFileLog();

    std::ifstream logFile(logDir + "/test.log");
    std::string content((std::istreambuf_iterator<char>(logFile)),
                         std::istreambuf_iterator<char>());

    CHECK(content.find("[WARN]") != std::string::npos, "应包含 [WARN] 标签");
    CHECK(content.find("[MyModule]") != std::string::npos, "应包含 [MyModule]");
    CHECK(content.find("格式测试消息") != std::string::npos, "应包含消息内容");

    cleanDir(logDir);
    PASS();
    return true;
}

static bool test_multi_thread_no_interleave() {
    TEST("多线程日志写入不产生交错行");
    http::Logger::shutdownFileLog();

    std::string logDir = "/tmp/test_logger_mt";
    cleanDir(logDir);
    mkdir(logDir.c_str(), 0755);

    http::Logger::initFileLog(logDir, "test.log");

    const int kThreads = 8;
    const int kPerThread = 50;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t]() {
            for (int i = 0; i < kPerThread; ++i) {
                LOG_INFO("MT", "线程-" << t << "-消息-" << i);
            }
        });
    }

    for (auto& th : threads) th.join();
    http::Logger::shutdownFileLog();

    std::ifstream logFile(logDir + "/test.log");
    std::string line;
    int lineCount = 0;
    int validCount = 0;

    while (std::getline(logFile, line)) {
        if (line.empty()) continue;
        lineCount++;
        if (line.find("[INFO][MT]") != std::string::npos &&
            line.find("线程-") != std::string::npos) {
            validCount++;
        }
    }

    CHECK(lineCount == kThreads * kPerThread,
          "期望 " << (kThreads*kPerThread) << " 行, 实际 " << lineCount);
    CHECK(validCount == kThreads * kPerThread,
          "所有行应具有正确格式");

    cleanDir(logDir);
    PASS();
    return true;
}

static bool test_shutdown_file_log() {
    TEST("shutdownFileLog 可安全重复调用");
    http::Logger::shutdownFileLog();
    http::Logger::shutdownFileLog();  // 不应崩溃
    LOG_INFO("TEST", "文件日志关闭后, 这条消息输出到 stderr");
    PASS();
    return true;
}

int main() {
    std::cout << "=== test_infra_logger ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) { g_testsPassed++; }
        else      { g_testsFailed++; }
    };

    run(test_level_filter_debug,          "DEBUG 级别过滤");
    run(test_level_filter_info_warn_error, "四个级别全部输出");
    run(test_file_log_output,             "文件日志输出");
    run(test_log_format,                  "日志格式");
    run(test_multi_thread_no_interleave,  "多线程不交错");
    run(test_shutdown_file_log,           "关闭文件日志");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
