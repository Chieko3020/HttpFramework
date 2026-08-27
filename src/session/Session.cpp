#include "session/Session.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace session {

Session::Session(const std::string& sessionId) 
    : sessionId_(sessionId), expirationTime_(std::chrono::seconds(3600)) { // 默认1小时过期
    touch();
}

void Session::set(const std::string& key, const std::string& value) {
    data_[key] = value;
    touch();
}

const std::string& Session::get(const std::string& key) const {
    static const std::string empty;
    auto it = data_.find(key);
    return (it != data_.end()) ? it->second : empty;
}

bool Session::has(const std::string& key) const {
    return data_.find(key) != data_.end();
}

void Session::remove(const std::string& key) {
    data_.erase(key);
    touch();
}

void Session::touch() {
    lastAccessTime_ = std::chrono::system_clock::now();
}

bool Session::isExpired() const {
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastAccessTime_);
    return elapsed >= expirationTime_;
}

void Session::setExpirationTime(std::chrono::seconds expirationTime) {
    expirationTime_ = expirationTime;
}

void Session::clear() {
    data_.clear();
    touch();
}

std::string Session::generateSessionId() {
    // 使用时间戳和随机数生成会话ID
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    std::stringstream ss;
    ss << std::hex << timestamp;
    
    // 添加随机字符
    for (int i = 0; i < 16; ++i) {
        ss << dis(gen);
    }
    
    return ss.str();
}

} // namespace session
