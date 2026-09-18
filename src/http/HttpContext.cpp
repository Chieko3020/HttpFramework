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
    clearCurrentResponse();
}

void HttpContext::resetForNextRequest() {
    // 注意：不清空请求缓冲——其中可能还有同一连接的下一个请求字节
    request_ = HttpRequest();
    response_ = HttpResponse();
    userData_.clear();

    writeOffset_ = 0;
    truncated_ = false;
    clearCurrentResponse();
}

void HttpContext::appendData(const std::string& data) {
    if (useMemoryPool_) {
        if (!requestBuffer_) {
            requestBuffer_ = std::make_unique<utils::PooledBuffer>(memoryPool_);
        }
        size_t written = requestBuffer_->write(data);
        if (written < data.size()) {
            truncated_ = true;
        }
    } else {
        buffer_ += data;
    }
}

void HttpContext::appendData(const char* data, size_t len) {
    if (useMemoryPool_) {
        if (!requestBuffer_) {
            requestBuffer_ = std::make_unique<utils::PooledBuffer>(memoryPool_);
        }
        size_t written = requestBuffer_->write(data, len);
        if (written < len) {
            truncated_ = true;
        }
    } else {
        buffer_.append(data, len);
    }
}

std::string HttpContext::getData() const {
    if (useMemoryPool_) {
        if (requestBuffer_) {
            return requestBuffer_->readString(requestBuffer_->getUsedSize());
        }
        return {};
    } else {
        return buffer_;
    }
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
}

void HttpContext::consumeData(size_t n) {
    if (useMemoryPool_) {
        if (requestBuffer_) {
            requestBuffer_->consume(n);
        }
    } else {
        if (n >= buffer_.size()) {
            buffer_.clear();
        } else {
            buffer_.erase(0, n);
        }
    }
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
