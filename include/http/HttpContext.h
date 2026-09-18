#pragma once

#include <atomic>
#include <chrono>
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
    
    // 清空上下文并重置连接状态（连接关闭时使用）
    void clear();

    // 长连接复用：清空请求/响应对象与响应缓冲，但保留 clientFd 与请求缓冲
    // （请求缓冲中可能已缓存同一连接的下一个请求——粘包 / pipelining）
    void resetForNextRequest();

    // 空闲超时判定用的活跃时间戳
    void touch() { lastActive_ = std::chrono::steady_clock::now(); }
    std::chrono::steady_clock::time_point lastActive() const { return lastActive_; }
    
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
    // 返回值：true = 响应完整落入缓冲区；false = 内存池单块装不下，已自动回退到
    // std::string 路径（内容仍然完整，只是内存来源不同）。绝不静默截断。
    bool setResponseData(const std::string& data);
    bool setResponseData(const char* data, size_t len);
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
    
    // 缓冲区截断检测（内存池单块容量不足时置位）
    bool isTruncated() const { return truncated_; }

    // 连接代际（见 utils/SocketCompat.h）：同一 fd 号被复用时用于区分新旧连接
    void setConnectionGeneration(uint32_t generation) { connectionGeneration_ = generation; }
    uint32_t connectionGeneration() const { return connectionGeneration_; }

    // ── 与业务线程之间的响应发布协议 ──
    // 响应由 worker 线程生成、由 I/O 线程读取/发送。裸读 std::string 与 worker 的
    // setResponseData() 构成数据竞争（可表现为 I/O 线程读到"暂时为空"的响应，
    // 从而既不发响应也不重新武装 EPOLLIN，请求永久丢失）。这里用
    // shared_ptr + atomic release/acquire 建立 happens-before：
    //   worker：setResponseData(...) → publishResponse(shared_ptr)（release）
    //   I/O   ：hasPendingResponse()（acquire）→ getResponseData()
    // I/O 线程读过一次后必须调用 releasePendingResponse()，否则会重复发送。
    void publishResponse(std::shared_ptr<const std::string> data) {
        pendingResponse_ = std::move(data);
        pendingResponseReady_.store(true, std::memory_order_release);
    }
    bool hasPendingResponse() const {
        return pendingResponseReady_.load(std::memory_order_acquire);
    }
    std::shared_ptr<const std::string> takePendingResponse() {
        pendingResponseReady_.store(false, std::memory_order_release);
        return pendingResponse_;
    }
    void dropPendingResponse() {
        pendingResponseReady_.store(false, std::memory_order_release);
        pendingResponse_.reset();
    }

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
    bool truncated_ = false;

    // 连接代际（同一 fd 号复用后的归属校验）
    uint32_t connectionGeneration_ = 0;

    // 响应发布协议（见头文件上半部分说明）
    std::shared_ptr<const std::string> pendingResponse_;
    std::atomic<bool> pendingResponseReady_{false};

    // 最近一次 I/O 活跃时间（空闲超时清理用）
    std::chrono::steady_clock::time_point lastActive_{std::chrono::steady_clock::now()};
};

} // namespace http
