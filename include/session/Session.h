#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <memory>

namespace session {

class Session {
public:
    using DataMap = std::unordered_map<std::string, std::string>;
    
    Session(const std::string& sessionId);
    ~Session() = default;
    
    // 获取会话ID
    const std::string& getId() const { return sessionId_; }
    
    // 设置和获取数据
    void set(const std::string& key, const std::string& value);
    const std::string& get(const std::string& key) const;
    bool has(const std::string& key) const;
    void remove(const std::string& key);
    
    // 获取所有数据
    const DataMap& getData() const { return data_; }
    
    // 会话生命周期管理
    void touch(); // 更新最后访问时间
    bool isExpired() const;
    std::chrono::system_clock::time_point getLastAccessTime() const { return lastAccessTime_; }
    
    // 设置过期时间
    void setExpirationTime(std::chrono::seconds expirationTime);
    std::chrono::seconds getExpirationTime() const { return expirationTime_; }
    
    // 清空会话数据
    void clear();
    
    // 检查会话是否有效
    bool isValid() const { return !sessionId_.empty() && !isExpired(); }

private:
    std::string sessionId_;
    DataMap data_;
    std::chrono::system_clock::time_point lastAccessTime_;
    std::chrono::seconds expirationTime_;
    
    // 生成会话ID
    static std::string generateSessionId();
};

} // namespace session
