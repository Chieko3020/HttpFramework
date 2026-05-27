#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include "Session.h"

namespace session {

class SessionManager {
public:
    explicit SessionManager(std::chrono::seconds defaultExpiration = std::chrono::seconds(3600)); // 默认1小时
    ~SessionManager();
    
    // 禁用拷贝构造和赋值
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;
    
    // 创建新会话
    std::shared_ptr<Session> createSession();
    std::shared_ptr<Session> createSession(const std::string& sessionId);
    
    // 获取会话
    std::shared_ptr<Session> getSession(const std::string& sessionId);
    
    // 删除会话
    void removeSession(const std::string& sessionId);
    
    // 检查会话是否存在
    bool hasSession(const std::string& sessionId) const;
    
    // 获取所有会话
    std::vector<std::shared_ptr<Session>> getAllSessions() const;
    
    // 清理过期会话
    void cleanupExpiredSessions();
    
    // 设置默认过期时间
    void setDefaultExpiration(std::chrono::seconds expiration);
    
    // 启动/停止清理线程
    void startCleanupThread(std::chrono::seconds cleanupInterval = std::chrono::seconds(300)); // 默认5分钟
    void stopCleanupThread();
    
    // 获取会话统计信息
    size_t getSessionCount() const;
    size_t getActiveSessionCount() const;

private:
    mutable std::mutex sessionsMutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    std::chrono::seconds defaultExpiration_;
    
    // 清理线程
    std::atomic<bool> cleanupThreadRunning_;
    std::thread cleanupThread_;
    std::chrono::seconds cleanupInterval_;
    std::condition_variable cleanupCondition_;
    std::mutex cleanupMutex_;
    
    // 清理线程函数
    void cleanupThreadFunction();
    
    // 生成唯一的会话ID
    std::string generateUniqueSessionId();
};

} // namespace session
