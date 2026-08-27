#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include "Session.h"

namespace session {

// 会话存储接口
class SessionStorage {
public:
    virtual ~SessionStorage() = default;
    
    // 保存会话
    virtual bool saveSession(std::shared_ptr<Session> session) = 0;
    
    // 加载会话
    virtual std::shared_ptr<Session> loadSession(const std::string& sessionId) = 0;
    
    // 删除会话
    virtual bool deleteSession(const std::string& sessionId) = 0;
    
    // 检查会话是否存在
    virtual bool hasSession(const std::string& sessionId) = 0;
    
    // 清理过期会话
    virtual void cleanupExpiredSessions() = 0;
};

// 内存存储实现
class MemorySessionStorage : public SessionStorage {
public:
    MemorySessionStorage();
    ~MemorySessionStorage() = default;
    
    bool saveSession(std::shared_ptr<Session> session) override;
    std::shared_ptr<Session> loadSession(const std::string& sessionId) override;
    bool deleteSession(const std::string& sessionId) override;
    bool hasSession(const std::string& sessionId) override;
    void cleanupExpiredSessions() override;
    
    // 获取存储统计信息
    size_t getStorageSize() const;

private:
    mutable std::mutex storageMutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> storage_;
};

// 文件存储实现
class FileSessionStorage : public SessionStorage {
public:
    explicit FileSessionStorage(const std::string& storagePath);
    ~FileSessionStorage() = default;
    
    bool saveSession(std::shared_ptr<Session> session) override;
    std::shared_ptr<Session> loadSession(const std::string& sessionId) override;
    bool deleteSession(const std::string& sessionId) override;
    bool hasSession(const std::string& sessionId) override;
    void cleanupExpiredSessions() override;

private:
    std::string storagePath_;
    mutable std::mutex storageMutex_;
    
    // 文件操作辅助函数
    std::string getSessionFilePath(const std::string& sessionId) const;
    bool writeSessionToFile(std::shared_ptr<Session> session, const std::string& filePath) const;
    std::shared_ptr<Session> readSessionFromFile(const std::string& filePath) const;
};

} // namespace session
