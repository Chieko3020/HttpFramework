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
    return writeSessionToFile(session, filePath);
}

std::shared_ptr<Session> FileSessionStorage::loadSession(const std::string& sessionId) {
    if (sessionId.empty()) {
        return nullptr;
    }
    
    std::string filePath = getSessionFilePath(sessionId);
    if (!std::filesystem::exists(filePath)) {
        return nullptr;
    }
    
    return readSessionFromFile(filePath);
}

bool FileSessionStorage::deleteSession(const std::string& sessionId) {
    if (sessionId.empty()) {
        return false;
    }
    
    std::string filePath = getSessionFilePath(sessionId);
    if (std::filesystem::exists(filePath)) {
        return std::filesystem::remove(filePath);
    }
    return false;
}

bool FileSessionStorage::hasSession(const std::string& sessionId) {
    if (sessionId.empty()) {
        return false;
    }
    
    std::string filePath = getSessionFilePath(sessionId);
    if (!std::filesystem::exists(filePath)) {
        return false;
    }
    
    auto session = readSessionFromFile(filePath);
    if (session && session->isExpired()) {
        std::filesystem::remove(filePath);
        return false;
    }
    
    return session != nullptr;
}

void FileSessionStorage::cleanupExpiredSessions() {
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

std::string FileSessionStorage::getSessionFilePath(const std::string& sessionId) const {
    return storagePath_ + "/" + sessionId + ".session";
}

bool FileSessionStorage::writeSessionToFile(std::shared_ptr<Session> session, const std::string& filePath) const {
    try {
        std::ofstream file(filePath);
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
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error writing session to file: " << e.what() << std::endl;
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
        
        // 恢复会话数据
        for (const auto& pair : data) {
            session->set(pair.first, pair.second);
        }
        
        return session;
    } catch (const std::exception& e) {
        std::cerr << "Error reading session from file: " << e.what() << std::endl;
        return nullptr;
    }
}

} // namespace session
