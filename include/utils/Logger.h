#pragma once

// 统一日志组件
// 用途：全框架统一的结构化日志输出，支持级别过滤和文件持久化
// 格式：[时间][级别][模块]：消息（文件输出含时间戳，控制台不含）
// 用法：LOG_INFO("HTTP", "服务启动, port=" << 8080);

#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

namespace http {

enum class LogLevel {
    Debug,  // 调试 — 细粒度诊断信息
    Info,   // 信息 — 关键流程事件
    Warn,   // 警告 — 非致命异常
    Error   // 错误 — 操作失败
};

class Logger {
public:
    // 设置最低输出级别：低于该级别的日志将被静默丢弃
    static void setMinLevel(LogLevel level);

    // 初始化文件日志：在 logDir 下创建目录并追加写入 logFile
    // 成功返回 true；失败时仍可在 stderr 输出
    static bool initFileLog(const std::string& logDir, const std::string& logFile = "server.log");

    // 关闭文件日志句柄
    static void shutdownFileLog();

    // 统一日志输出（供宏调用）
    static void log(LogLevel level, const std::string& module, const std::string& message);

private:
    static const char* levelLabel(LogLevel level);
    static std::string nowString();

    static std::mutex mu_;
    static LogLevel minLevel_;
    static std::unique_ptr<std::ofstream> fileOut_;
};

}  // namespace http

// ── 便捷宏 ──────────────────────────────────────────────────────────

#define LOG_DEBUG(module, msg) do { \
    std::ostringstream _oss; _oss << msg; \
    http::Logger::log(http::LogLevel::Debug, module, _oss.str()); \
} while(0)

#define LOG_INFO(module, msg) do { \
    std::ostringstream _oss; _oss << msg; \
    http::Logger::log(http::LogLevel::Info, module, _oss.str()); \
} while(0)

#define LOG_WARN(module, msg) do { \
    std::ostringstream _oss; _oss << msg; \
    http::Logger::log(http::LogLevel::Warn, module, _oss.str()); \
} while(0)

#define LOG_ERROR(module, msg) do { \
    std::ostringstream _oss; _oss << msg; \
    http::Logger::log(http::LogLevel::Error, module, _oss.str()); \
} while(0)
