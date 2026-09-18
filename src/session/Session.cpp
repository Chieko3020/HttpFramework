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
    std::lock_guard<std::mutex> lk(mutex_);
    data_[key] = value;
    touchLocked();
}

std::string Session::get(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = data_.find(key);
    return (it != data_.end()) ? it->second : std::string();
}

bool Session::has(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return data_.find(key) != data_.end();
}

void Session::remove(const std::string& key) {
    std::lock_guard<std::mutex> lk(mutex_);
    data_.erase(key);
    touchLocked();
}

void Session::touch() {
    std::lock_guard<std::mutex> lk(mutex_);
    touchLocked();
}

void Session::touchLocked() {
    lastAccessTime_ = std::chrono::system_clock::now();
}

bool Session::isExpired() const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastAccessTime_);
    return elapsed >= expirationTime_;
}

std::chrono::system_clock::time_point Session::getLastAccessTime() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return lastAccessTime_;
}

void Session::setLastAccessTime(std::chrono::system_clock::time_point t) {
    std::lock_guard<std::mutex> lk(mutex_);
    lastAccessTime_ = t;
}

void Session::setExpirationTime(std::chrono::seconds expirationTime) {
    std::lock_guard<std::mutex> lk(mutex_);
    expirationTime_ = expirationTime;
}

std::chrono::seconds Session::getExpirationTime() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return expirationTime_;
}

std::chrono::seconds Session::remainingTime() const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastAccessTime_);
    auto rem = expirationTime_ - elapsed;
    return rem.count() > 0 ? rem : std::chrono::seconds(0);
}

Session::DataMap Session::getData() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return data_;
}

void Session::clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    data_.clear();
    touchLocked();
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
