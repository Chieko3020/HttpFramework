// 统一日志组件实现

#include "utils/Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>

namespace http {

std::mutex Logger::mu_;
LogLevel Logger::minLevel_ = LogLevel::Info;
std::unique_ptr<std::ofstream> Logger::fileOut_;

namespace {

bool ensureDir(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    if (::mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
}

}  // namespace

void Logger::setMinLevel(LogLevel level) {
    std::lock_guard<std::mutex> lk(mu_);
    minLevel_ = level;
}

bool Logger::initFileLog(const std::string& logDir, const std::string& logFile) {
    std::lock_guard<std::mutex> lk(mu_);
    fileOut_.reset();
    if (logDir.empty() || logFile.empty()) return false;
    if (!ensureDir(logDir)) return false;
    std::string path = logDir + "/" + logFile;
    auto out = std::make_unique<std::ofstream>(path, std::ios::app | std::ios::out);
    if (!out->good()) return false;
    fileOut_ = std::move(out);
    return true;
}

void Logger::shutdownFileLog() {
    std::lock_guard<std::mutex> lk(mu_);
    fileOut_.reset();
}

const char* Logger::levelLabel(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        default:              return "UNKNOWN";
    }
}

std::string Logger::nowString() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::tm tm_buf;
    localtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
        << "." << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

void Logger::log(LogLevel level, const std::string& module, const std::string& message) {
    std::lock_guard<std::mutex> lk(mu_);

    // 级别过滤
    if (static_cast<int>(level) < static_cast<int>(minLevel_)) return;

    // 控制台输出：[LEVEL][MODULE]：message
    std::ostringstream consoleLine;
    consoleLine << "[" << levelLabel(level) << "]"
                << "[" << module << "]：" << message << "\n";
    std::cerr << consoleLine.str();

    // 文件输出：[时间][LEVEL][MODULE]：message
    if (fileOut_ && fileOut_->good()) {
        std::ostringstream fileLine;
        fileLine << "[" << nowString() << "]"
                 << "[" << levelLabel(level) << "]"
                 << "[" << module << "]：" << message << "\n";
        *fileOut_ << fileLine.str();
        fileOut_->flush();
    }
}

}  // namespace http
