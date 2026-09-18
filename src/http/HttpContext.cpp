#include "http/HttpContext.h"
#include <iostream>

namespace http {

HttpContext::HttpContext()
    : clientFd_(-1), keepAlive_(false), useMemoryPool_(false),
      memoryPool_(nullptr), writeOffset_(0) {
}

void HttpContext::setUserData(const std::string& key, const std::string& value) {
    userData_[key] = value;
}

const std::string& HttpContext::getUserData(const std::string& key) const {
    static const std::string empty;
    auto it = userData_.find(key);
    return (it != userData_.end()) ? it->second : empty;
}

bool HttpContext::hasUserData(const std::string& key) const {
    return userData_.find(key) != userData_.end();
}

void HttpContext::clear() {
    request_ = HttpRequest();
    response_ = HttpResponse();
    userData_.clear();
    clientFd_ = -1;
    keepAlive_ = false;
    
    // 清空缓冲区
    if (useMemoryPool_) {
        if (requestBuffer_) {
            requestBuffer_->clear();
        }
    }
    buffer_.clear();
    
    // 重置写入偏移量与截断标志
    writeOffset_ = 0;
    truncated_ = false;
    allocationFailed_ = false;
    requestTooLarge_ = false;
    readPos_ = 0;
    clearCurrentResponse();
}

void HttpContext::resetForNextRequest() {
    // 注意：不清空请求缓冲——其中可能还有同一连接的下一个请求字节
    request_ = HttpRequest();
    response_ = HttpResponse();
    userData_.clear();

    writeOffset_ = 0;
    truncated_ = false;
    allocationFailed_ = false;
    requestTooLarge_ = false;
    clearCurrentResponse();
}

// 内存池模式下把数据写入请求缓冲。三种结局必须可区分（H7/H8）：
//   全部写入        → 正常
//   写不满（有块）  → truncated_：请求体超过单块容量 → 413
//   完全写不进（无块/池耗尽）→ allocationFailed_：服务端容量不足 → 503
// 旧实现把后两种都当成"截断"，池耗尽时会回一个语义错误的 413。
// 把待写入的数据落到真实缓冲区。写之前先把已消费的读游标压缩掉
// （每次读事件最多一次 memmove，而不是每次 consume 都搬），并维护
// "半包起始时刻"与"超过配置上限"两个状态（M1/M4）。
void HttpContext::appendData(const std::string& data) {
    appendData(data.data(), data.size());
}

void HttpContext::appendData(const char* data, size_t len) {
    if (len == 0) return;
    const bool wasEmpty = (dataSize() == 0);

    if (useMemoryPool_) {
        if (!requestBuffer_) {
            requestBuffer_ = std::make_unique<utils::PooledBuffer>(memoryPool_);
        }
        if (!requestBuffer_->valid()) {
            allocationFailed_ = true;
            return;
        }
        if (readPos_ > 0) {
            requestBuffer_->consume(readPos_);   // 压缩一次
            readPos_ = 0;
        }
        size_t written = requestBuffer_->write(data, len);
        if (written < len) {
            truncated_ = true;
        }
    } else {
        if (readPos_ > 0) {
            buffer_.erase(0, readPos_);
            readPos_ = 0;
        }
        buffer_.append(data, len);
        // 请求缓冲上限：慢速滴灌不能把单连接内存拖到无界（M4）
        if (buffer_.size() > maxRequestBytes_) {
            requestTooLarge_ = true;
        }
    }

    if (wasEmpty) {
        requestStart_ = std::chrono::steady_clock::now();
    }
}

std::string_view HttpContext::peekData() const {
    if (useMemoryPool_) {
        if (!requestBuffer_) return {};
        const char* p = requestBuffer_->rawData();
        const size_t n = requestBuffer_->getUsedSize();
        if (!p || readPos_ >= n) return {};
        return std::string_view(p + readPos_, n - readPos_);
    }
    if (readPos_ >= buffer_.size()) return {};
    return std::string_view(buffer_.data() + readPos_, buffer_.size() - readPos_);
}

std::string HttpContext::getData() const {
    // 兼容接口：返回剩余数据的整块拷贝（内部路径已改用 peekData()）
    std::string_view v = peekData();
    return std::string(v.data(), v.size());
}

size_t HttpContext::requestBufferCapacity() const {
    if (useMemoryPool_) {
        return requestBuffer_ ? requestBuffer_->size()
                              : utils::HttpMemoryPool::BLOCK_SIZE;
    }
    // 非池模式没有固定块容量，返回 0 表示"仅受 HttpServer 的请求缓冲上限约束"
    return 0;
}

void HttpContext::clearData() {
    if (useMemoryPool_) {
        if (requestBuffer_) {
            requestBuffer_->clear();
        }
    } else {
        buffer_.clear();
    }
    readPos_ = 0;
}

void HttpContext::consumeData(size_t n) {
    // 只推进读游标（零拷贝）；缓冲区的物理回收推迟到下一次 appendData（M1）
    const size_t size = dataSize();
    if (n >= size) {
        if (useMemoryPool_) {
            if (requestBuffer_) requestBuffer_->clear();
        } else {
            buffer_.clear();
        }
        readPos_ = 0;
        return;
    }
    readPos_ += n;
}

void HttpContext::enableMemoryPool(bool enable, utils::HttpMemoryPool* pool) {
    if (useMemoryPool_ == enable && (!enable || pool == nullptr || memoryPool_ == pool)) {
        return;  // 状态没有变化
    }
    
    useMemoryPool_ = enable;
    
    if (enable) {
        // 指定池优先（HttpServer 自持的池，块容量可配置）；
        // 未指定时回退到进程级全局池（避免未使用时分配 60MB）
        memoryPool_ = pool ? pool : &utils::GlobalMemoryPool::getInstance();
        if (!memoryPool_) {
            useMemoryPool_ = false;
            return;
        }
        // 从传统模式切换到内存池模式
        if (!buffer_.empty()) {
            if (!requestBuffer_) {
                requestBuffer_ = std::make_unique<utils::PooledBuffer>(memoryPool_);
            }
            requestBuffer_->write(buffer_);
            buffer_.clear();
        }
    } else {
        // 从内存池模式切换到传统模式：先搬内容再析构池块
        if (requestBuffer_) {
            buffer_ = requestBuffer_->readString(requestBuffer_->getUsedSize());
            requestBuffer_.reset();
        }
    }
}

void HttpContext::printMemoryPoolStats() const {
    std::cout << "HttpContext Memory Pool Stats:" << std::endl;
    std::cout << "  Memory Pool Enabled: " << (useMemoryPool_ ? "Yes" : "No") << std::endl;
    std::cout << "  Request Buffer Size: " << (useMemoryPool_ && requestBuffer_ ? 
        requestBuffer_->getUsedSize() : buffer_.size()) << " bytes" << std::endl;
    
    if (useMemoryPool_) {
        std::cout << "  Pool Total Blocks: " << memoryPool_->getTotalBlocks() << std::endl;
        std::cout << "  Pool Used Blocks: " << memoryPool_->getUsedBlocks() << std::endl;
        std::cout << "  Pool Available Blocks: " << memoryPool_->getAvailableBlocks() << std::endl;
    }
}

} // namespace http
