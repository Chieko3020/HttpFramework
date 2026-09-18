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

    // 请求缓冲容量（内存池模式下即池的单块容量）；用于在 413 里给出可区分的上限（H7）
    size_t requestBufferCapacity() const;

    // 内存池管理。pool 为 nullptr 时使用进程级全局池（向后兼容）；
    // HttpServer 启用内存池时会传入自己持有的池实例，使块容量可配置（H7）
    void enableMemoryPool(bool enable = true, utils::HttpMemoryPool* pool = nullptr);
    bool isMemoryPoolEnabled() const { return useMemoryPool_; }
    
    // 获取内存池统计信息
    void printMemoryPoolStats() const;
    
    // 写入进度管理
    void resetWriteOffset() { writeOffset_ = 0; }
    size_t getWriteOffset() const { return writeOffset_; }
    void setWriteOffset(size_t offset) { writeOffset_ = offset; }
    
    // 缓冲区截断检测（内存池单块容量不足时置位）
    bool isTruncated() const { return truncated_; }

    // 内存池分配失败（池耗尽）：请求缓冲根本没拿到块，写入量为 0。
    // 这是"服务端容量不足"（应回 503），与"请求体超过单块容量"（413）语义不同（H8）
    bool isAllocationFailed() const { return allocationFailed_; }

    // ── 在途请求计数（H4）──
    // "该连接上有一个请求已被派发给业务线程、响应尚未发出"。
    // 空闲超时清理必须跳过这类连接：慢 handler（超过 idleTimeout）不代表连接空闲，
    // 关掉它既会杀掉正常请求，也会打开 fd 复用后被陈旧回调误杀的窗口。
    // 计数由 I/O 线程在派发时递增（含"已入队未开始"），由任务结束时递减。
    void enterInFlight() { inFlight_.fetch_add(1, std::memory_order_relaxed); }
    void leaveInFlight() {
        int prev = inFlight_.fetch_sub(1, std::memory_order_relaxed);
        if (prev <= 0) {
            // 计数不应为负：为负说明配对错误，钳回 0 避免永久"看起来在途"
            inFlight_.store(0, std::memory_order_relaxed);
        }
    }
    int inFlightCount() const { return inFlight_.load(std::memory_order_relaxed); }

    // 连接代际（见 utils/SocketCompat.h）：同一 fd 号被复用时用于区分新旧连接
    void setConnectionGeneration(uint32_t generation) { connectionGeneration_ = generation; }
    uint32_t connectionGeneration() const { return connectionGeneration_; }

    // ── 响应区（H5）──
    // 重要约定：**只有 I/O 线程触碰**这些状态。
    // 业务（worker）线程只把"要发送的字节"投递到 subReactor 的响应队列，
    // 由 I/O 线程取出后写入这里。这样 ctx 上不再有任何跨线程字段
    // （此前的 publishResponse 虽然用 release/acquire 发布，但 shared_ptr 本身的
    //  写与读仍是竞争，只能靠"同一连接只有一个在途响应"的契约兜住）。
    void setCurrentResponse(std::shared_ptr<const std::string> data) {
        currentResponse_ = std::move(data);
    }
    bool hasCurrentResponse() const { return static_cast<bool>(currentResponse_); }
    const std::shared_ptr<const std::string>& currentResponse() const {
        return currentResponse_;
    }
    void clearCurrentResponse() { currentResponse_.reset(); }

private:
    HttpRequest request_;
    HttpResponse response_;
    std::unordered_map<std::string, std::string> userData_;
    int clientFd_;
    bool keepAlive_;
    
    // 传统缓冲区（向后兼容）
    std::string buffer_;        // 请求数据缓冲区

    // 内存池缓冲区（仅请求方向；响应方向不再使用固定块池，见 H5）
    bool useMemoryPool_;
    std::unique_ptr<utils::PooledBuffer> requestBuffer_;
    
    // 全局内存池指针 (延迟初始化，避免未使用时分配 60MB)
    utils::HttpMemoryPool* memoryPool_;
    
    // 写入进度跟踪
    size_t writeOffset_;
    bool truncated_ = false;
    bool allocationFailed_ = false;

    // 在途请求计数（H4，见上方 enterInFlight 说明）
    std::atomic<int> inFlight_{0};

    // 连接代际（同一 fd 号复用后的归属校验）
    uint32_t connectionGeneration_ = 0;

    // 当前正在发送的响应（I/O 线程独占，见上方响应区说明）
    std::shared_ptr<const std::string> currentResponse_;

    // 最近一次 I/O 活跃时间（空闲超时清理用）
    std::chrono::steady_clock::time_point lastActive_{std::chrono::steady_clock::now()};
};

} // namespace http
