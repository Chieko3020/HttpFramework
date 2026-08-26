#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "utils/MemoryPool.h"

namespace http {

class HttpContext {
public:
    HttpContext();
    ~HttpContext() = default;
    
    // 获取请求对象
    HttpRequest& getRequest() { return request_; }
    const HttpRequest& getRequest() const { return request_; }
    
    // 获取响应对象
    HttpResponse& getResponse() { return response_; }
    const HttpResponse& getResponse() const { return response_; }
    
    // 设置和获取用户数据
    void setUserData(const std::string& key, const std::string& value);
    const std::string& getUserData(const std::string& key) const;
    bool hasUserData(const std::string& key) const;
    
    // 清空上下文
    void clear();
    
    // 设置客户端文件描述符
    void setClientFd(int fd) { clientFd_ = fd; }
    int getClientFd() const { return clientFd_; }
    
    // 设置是否保持连接
    void setKeepAlive(bool keepAlive) { keepAlive_ = keepAlive; }
    bool isKeepAlive() const { return keepAlive_; }
    
    // 数据缓冲区管理 - 支持内存池和传统方式
    void appendData(const std::string& data);
    void appendData(const char* data, size_t len);
    std::string getData() const;
    void clearData();
    void consumeData(size_t n);  // 只消费前 n 字节，保留剩余数据

    // 响应数据管理 - 支持内存池和传统方式
    void setResponseData(const std::string& data);
    void setResponseData(const char* data, size_t len);
    std::string getResponseData() const;
    
    // 内存池管理
    void enableMemoryPool(bool enable = true);
    bool isMemoryPoolEnabled() const { return useMemoryPool_; }
    
    // 获取内存池统计信息
    void printMemoryPoolStats() const;
    
    // 写入进度管理
    void resetWriteOffset() { writeOffset_ = 0; }
    size_t getWriteOffset() const { return writeOffset_; }
    void setWriteOffset(size_t offset) { writeOffset_ = offset; }
    bool hasMoreDataToWrite() const;

private:
    HttpRequest request_;
    HttpResponse response_;
    std::unordered_map<std::string, std::string> userData_;
    int clientFd_;
    bool keepAlive_;
    
    // 传统缓冲区（向后兼容）
    std::string buffer_;        // 请求数据缓冲区
    std::string responseData_;  // 响应数据缓冲区
    
    // 内存池缓冲区
    bool useMemoryPool_;
    std::unique_ptr<utils::PooledBuffer> requestBuffer_;
    std::unique_ptr<utils::PooledBuffer> responseBuffer_;
    
    // 全局内存池指针 (延迟初始化，避免未使用时分配 60MB)
    utils::HttpMemoryPool* memoryPool_;
    
    // 写入进度跟踪
    size_t writeOffset_;
};

} // namespace http
