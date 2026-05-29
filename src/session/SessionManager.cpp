#include "session/SessionManager.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <iostream>

namespace session {

SessionManager::SessionManager(std::chrono::seconds defaultExpiration)
    : defaultExpiration_(defaultExpiration), cleanupThreadRunning_(false) {
}

SessionManager::~SessionManager() {
    stopCleanupThread();
}

std::shared_ptr<Session> SessionManager::createSession() {
    std::string sessionId = generateUniqueSessionId();
    return createSession(sessionId);
}

std::shared_ptr<Session> SessionManager::createSession(const std::string& sessionId) {
    auto session = std::make_shared<Session>(sessionId);
    session->setExpirationTime(defaultExpiration_);
    
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessions_[sessionId] = session;
    
    return session;
}

std::shared_ptr<Session> SessionManager::getSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        auto session = it->second;
        if (session->isExpired()) {
            sessions_.erase(it);
            return nullptr;
        }
        session->touch(); // 更新访问时间
        return session;
    }
    return nullptr;
}

void SessionManager::removeSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessions_.erase(sessionId);
}

bool SessionManager::hasSession(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        if (it->second->isExpired()) {
            return false;
        }
        return true;
    }
    return false;
}

std::vector<std::shared_ptr<Session>> SessionManager::getAllSessions() const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    std::vector<std::shared_ptr<Session>> result;
    
    for (const auto& pair : sessions_) {
        if (!pair.second->isExpired()) {
            result.push_back(pair.second);
        }
    }
    
    return result;
}

void SessionManager::cleanupExpiredSessions() {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    
    auto it = sessions_.begin();
    while (it != sessions_.end()) {
        if (it->second->isExpired()) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void SessionManager::setDefaultExpiration(std::chrono::seconds expiration) {
    defaultExpiration_ = expiration;
}

void SessionManager::startCleanupThread(std::chrono::seconds cleanupInterval) {
    if (cleanupThreadRunning_.load()) {
        return;
    }
    
    cleanupInterval_ = cleanupInterval;
    cleanupThreadRunning_.store(true);
    cleanupThread_ = std::thread(&SessionManager::cleanupThreadFunction, this);
    
    std::cout << "[INFO][会话]：清理线程已启动, 间隔=" << cleanupInterval.count() << "秒" << std::endl;
}

void SessionManager::stopCleanupThread() {
    if (!cleanupThreadRunning_.load()) {
        return;
    }
    
    cleanupThreadRunning_.store(false);
    cleanupCondition_.notify_all();  // 唤醒等待的线程
    
    if (cleanupThread_.joinable()) {
        cleanupThread_.join();
    }
    
    std::cout << "[INFO][会话]：清理线程已停止" << std::endl;
}

size_t SessionManager::getSessionCount() const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    return sessions_.size();
}

size_t SessionManager::getActiveSessionCount() const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    size_t count = 0;
    for (const auto& pair : sessions_) {
        if (!pair.second->isExpired()) {
            count++;
        }
    }
    return count;
}

void SessionManager::cleanupThreadFunction() {
    std::unique_lock<std::mutex> lock(cleanupMutex_);
    
    while (cleanupThreadRunning_.load()) {
        // 使用条件变量等待，可以被立即唤醒
        if (cleanupCondition_.wait_for(lock, cleanupInterval_, [this] { return !cleanupThreadRunning_.load(); })) {
            // 被唤醒且需要退出
            break;
        }
        
        // 超时或被唤醒但需要继续运行
        if (cleanupThreadRunning_.load()) {
            cleanupExpiredSessions();
            std::cout << "[DEBUG][会话]：清理完成, 活跃会话数=" << getActiveSessionCount() << std::endl;
        }
    }
}

std::string SessionManager::generateUniqueSessionId() {
    // 生成唯一的会话ID
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
    
    std::string sessionId = ss.str();
    
    // 确保ID唯一性
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    while (sessions_.find(sessionId) != sessions_.end()) {
        sessionId += std::to_string(dis(gen));
    }
    
    return sessionId;
}

} // namespace session
