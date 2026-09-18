#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <memory>
#include <mutex>

namespace session {

class Session {
public:
    using DataMap = std::unordered_map<std::string, std::string>;
    
    Session(const std::string& sessionId);
    ~Session() = default;
    
    // 获取会话ID
    const std::string& getId() const { return sessionId_; }
    
    // 设置和获取数据。
    // 方法内部持锁（M12）：同一个 session 可能被多个请求线程并发访问，
    // SessionManager 的锁只覆盖"取到 shared_ptr"这一步，不覆盖 Session 内部。
    void set(const std::string& key, const std::string& value);
    std::string get(const std::string& key) const;
    bool has(const std::string& key) const;
    void remove(const std::string& key);
    
    // 获取所有数据（返回副本：持锁期间无法安全地把内部容器引用交给调用方）
    DataMap getData() const;
    
    // 会话生命周期管理
    void touch(); // 更新最后访问时间
    bool isExpired() const;
    std::chrono::system_clock::time_point getLastAccessTime() const;
    // 从持久化存储恢复最后访问时间（M12）
    void setLastAccessTime(std::chrono::system_clock::time_point t);
    
    // 设置过期时间
    void setExpirationTime(std::chrono::seconds expirationTime);
    std::chrono::seconds getExpirationTime() const;
    // 距过期还剩多久（Cookie 的 Max-Age 据此刷新，M12）
    std::chrono::seconds remainingTime() const;
    
    // 清空会话数据
    void clear();
    
    // 检查会话是否有效
    bool isValid() const { return !sessionId_.empty() && !isExpired(); }

private:
    std::string sessionId_;
    mutable std::mutex mutex_;
    DataMap data_;
    std::chrono::system_clock::time_point lastAccessTime_;
    std::chrono::seconds expirationTime_;

    // 持锁内部版本（公开方法在同一把锁内调用，避免自死锁）
    void touchLocked();
    
    // 生成会话ID
    static std::string generateSessionId();
};

} // namespace session
