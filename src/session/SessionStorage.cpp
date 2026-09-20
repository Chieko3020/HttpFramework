#include "session/SessionStorage.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <chrono>

namespace session {

// MemorySessionStorage 实现
MemorySessionStorage::MemorySessionStorage() {
}

bool MemorySessionStorage::saveSession(std::shared_ptr<Session> session) {
    if (!session || session->getId().empty()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(storageMutex_);
    storage_[session->getId()] = session;
    return true;
}

std::shared_ptr<Session> MemorySessionStorage::loadSession(const std::string& sessionId) {
    if (sessionId.empty()) {
        return nullptr;
    }
    
    std::lock_guard<std::mutex> lock(storageMutex_);
    auto it = storage_.find(sessionId);
    if (it != storage_.end()) {
        auto session = it->second;
        if (session->isExpired()) {
            storage_.erase(it);
            return nullptr;
        }
        return session;
    }
    return nullptr;
}

bool MemorySessionStorage::deleteSession(const std::string& sessionId) {
    if (sessionId.empty()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(storageMutex_);
    auto it = storage_.find(sessionId);
    if (it != storage_.end()) {
        storage_.erase(it);
        return true;
    }
    return false;
}

bool MemorySessionStorage::hasSession(const std::string& sessionId) {
    if (sessionId.empty()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(storageMutex_);
    auto it = storage_.find(sessionId);
    if (it != storage_.end()) {
        if (it->second->isExpired()) {
            storage_.erase(it);
            return false;
        }
        return true;
    }
    return false;
}

void MemorySessionStorage::cleanupExpiredSessions() {
    std::lock_guard<std::mutex> lock(storageMutex_);
    
    auto it = storage_.begin();
    while (it != storage_.end()) {
        if (it->second->isExpired()) {
            it = storage_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t MemorySessionStorage::getStorageSize() const {
    std::lock_guard<std::mutex> lock(storageMutex_);
    return storage_.size();
}

// FileSessionStorage 实现
FileSessionStorage::FileSessionStorage(const std::string& storagePath) 
    : storagePath_(storagePath) {
    
    // 确保存储目录存在
    std::filesystem::create_directories(storagePath_);
}

bool FileSessionStorage::saveSession(std::shared_ptr<Session> session) {
    if (!session || session->getId().empty()) {
        return false;
    }
    
    std::string filePath = getSessionFilePath(session->getId());
    if (filePath.empty()) {
        std::cerr << "[WARN][会话]：非法会话 ID，拒绝写入文件" << std::endl;
        return false;
    }
    std::lock_guard<std::mutex> lock(storageMutex_);   // 同一会话文件的写与清理互斥（M12）
    return writeSessionToFile(session, filePath);
}

std::shared_ptr<Session> FileSessionStorage::loadSession(const std::string& sessionId) {
    if (sessionId.empty()) return nullptr;
    
    std::string filePath = getSessionFilePath(sessionId);
    if (filePath.empty()) return nullptr;
    if (!std::filesystem::exists(filePath)) return nullptr;
    
    std::lock_guard<std::mutex> lock(storageMutex_);
    return readSessionFromFile(filePath);
}

bool FileSessionStorage::deleteSession(const std::string& sessionId) {
    if (sessionId.empty()) return false;
    
    std::string filePath = getSessionFilePath(sessionId);
    if (filePath.empty()) return false;
    if (std::filesystem::exists(filePath)) {
        std::lock_guard<std::mutex> lock(storageMutex_);
        return std::filesystem::remove(filePath);
    }
    return false;
}

bool FileSessionStorage::hasSession(const std::string& sessionId) {
    if (sessionId.empty()) return false;
    
    std::string filePath = getSessionFilePath(sessionId);
    if (filePath.empty()) return false;
    if (!std::filesystem::exists(filePath)) return false;
    
    std::lock_guard<std::mutex> lock(storageMutex_);
    auto session = readSessionFromFile(filePath);
    if (session && session->isExpired()) {
        std::filesystem::remove(filePath);
        return false;
    }
    
    return session != nullptr;
}

void FileSessionStorage::cleanupExpiredSessions() {
    std::lock_guard<std::mutex> lock(storageMutex_);
    try {
        for (const auto& entry : std::filesystem::directory_iterator(storagePath_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".session") {
                auto session = readSessionFromFile(entry.path().string());
                if (!session || session->isExpired()) {
                    std::filesystem::remove(entry.path());
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error cleaning up expired sessions: " << e.what() << std::endl;
    }
}

// sessionId 白名单校验（M12）：会话 ID 直接来自 Cookie，参与路径拼接，
// 不校验就会被 "../" 穿越出存储目录读写任意文件。
// 只允许 [A-Za-z0-9_-]，长度 16~64（本框架生成的 ID 是十六进制，满足该约束）。
static bool isValidSessionId(const std::string& id) {
    if (id.size() < 16 || id.size() > 64) return false;
    for (unsigned char c : id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string FileSessionStorage::getSessionFilePath(const std::string& sessionId) const {
    if (!isValidSessionId(sessionId)) {
        return std::string();   // 调用方必须检查空路径
    }
    return storagePath_ + "/" + sessionId + ".session";
}

bool FileSessionStorage::writeSessionToFile(std::shared_ptr<Session> session, const std::string& filePath) const {
    try {
        // 原子写（M12）：先写临时文件再 rename。
        // 直接覆盖原文件的话，读到"写了一半"的文件就解析失败 → 会话被当成不存在。
        const std::string tmpPath = filePath + ".tmp";
        std::ofstream file(tmpPath, std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }
        
        // 写入会话基本信息
        file << "SESSION_ID:" << session->getId() << "\n";
        file << "LAST_ACCESS:" << std::chrono::duration_cast<std::chrono::seconds>(
            session->getLastAccessTime().time_since_epoch()).count() << "\n";
        file << "EXPIRATION:" << session->getExpirationTime().count() << "\n";
        
        // 写入会话数据
        file << "DATA_START\n";
        for (const auto& pair : session->getData()) {
            file << pair.first << ":" << pair.second << "\n";
        }
        file << "DATA_END\n";
        
        file.close();
        std::error_code ec;
        std::filesystem::rename(tmpPath, filePath, ec);
        if (ec) {
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR][会话]：会话写入文件失败: " << e.what() << std::endl;
        return false;
    }
}

std::shared_ptr<Session> FileSessionStorage::readSessionFromFile(const std::string& filePath) const {
    try {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            return nullptr;
        }
        
        std::string line;
        std::string sessionId;
        std::chrono::system_clock::time_point lastAccess;
        std::chrono::seconds expiration;
        std::unordered_map<std::string, std::string> data;
        
        bool inDataSection = false;
        
        while (std::getline(file, line)) {
            if (line == "DATA_START") {
                inDataSection = true;
                continue;
            } else if (line == "DATA_END") {
                break;
            }
            
            if (!inDataSection) {
                size_t colonPos = line.find(':');
                if (colonPos != std::string::npos) {
                    std::string key = line.substr(0, colonPos);
                    std::string value = line.substr(colonPos + 1);
                    
                    if (key == "SESSION_ID") {
                        sessionId = value;
                    } else if (key == "LAST_ACCESS") {
                        auto timestamp = std::chrono::seconds(std::stoll(value));
                        lastAccess = std::chrono::system_clock::time_point(timestamp);
                    } else if (key == "EXPIRATION") {
                        expiration = std::chrono::seconds(std::stoll(value));
                    }
                }
            } else {
                size_t colonPos = line.find(':');
                if (colonPos != std::string::npos) {
                    std::string key = line.substr(0, colonPos);
                    std::string value = line.substr(colonPos + 1);
                    data[key] = value;
                }
            }
        }
        
        file.close();
        
        if (sessionId.empty()) {
            return nullptr;
        }
        
        auto session = std::make_shared<Session>(sessionId);
        session->setExpirationTime(expiration);
        // 恢复最后访问时间（M12）：不恢复的话，重启后所有会话的滑动窗口都从"文件
        // 里的 EXPIRATION"重新起算，长会话会被提前判过期
        if (lastAccess.time_since_epoch().count() != 0) {
            session->setLastAccessTime(lastAccess);
        }
        
        // 恢复会话数据
        for (const auto& pair : data) {
            session->set(pair.first, pair.second);
        }
        
        return session;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR][会话]：从文件读取会话失败: " << e.what() << std::endl;
        return nullptr;
    }
}

} // namespace session
