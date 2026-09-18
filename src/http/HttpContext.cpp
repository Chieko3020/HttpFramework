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
        if (responseBuffer_) {
            responseBuffer_->clear();
        }
    }
    buffer_.clear();
    responseData_.clear();
    
    // 重置写入偏移量与截断标志
    writeOffset_ = 0;
    truncated_ = false;
    dropPendingResponse();
}

void HttpContext::resetForNextRequest() {
    // 注意：不清空请求缓冲——其中可能还有同一连接的下一个请求字节
    request_ = HttpRequest();
    response_ = HttpResponse();
    userData_.clear();

    if (useMemoryPool_) {
        if (responseBuffer_) {
            responseBuffer_->clear();
        }
    }
    responseData_.clear();

    writeOffset_ = 0;
    truncated_ = false;
    dropPendingResponse();
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

bool HttpContext::setResponseData(const std::string& data) {
    truncated_ = false;
    if (useMemoryPool_) {
        // 池内单块容量有限（BLOCK_SIZE）。写不满时必须回退到 std::string，
        // 否则响应体会被静默截断，而 Content-Length 仍声明完整长度，
        // keep-alive 下后续响应会被当成前一响应的续传体（长连接整体错位）。
        if (!responseBuffer_) {
            responseBuffer_ = std::make_unique<utils::PooledBuffer>(memoryPool_);
        }
        responseBuffer_->clear();
        size_t written = responseBuffer_->write(data);
        if (written < data.size()) {
            responseBuffer_.reset();          // 归还池块，避免白占
            responseData_ = data;
            truncated_ = true;
            return false;
        }
        responseData_.clear();
        return true;
    }

    // 内存池未启用，使用传统方式
    responseData_ = data;
    return true;
}

bool HttpContext::setResponseData(const char* data, size_t len) {
    return setResponseData(std::string(data, len));
}

std::string HttpContext::getResponseData() const {
    if (!responseData_.empty()) {
        // 池装不下时的完整响应（以及非池模式下的响应）
        return responseData_;
    }
    if (useMemoryPool_) {
        if (responseBuffer_) {
            return responseBuffer_->readString(responseBuffer_->getUsedSize());
        }
    }
    return responseData_;
}

void HttpContext::enableMemoryPool(bool enable) {
    if (useMemoryPool_ == enable) {
        return;  // 状态没有变化
    }
    
    useMemoryPool_ = enable;
    
    if (enable) {
        // 延迟初始化全局内存池 (避免未使用时分配 60MB)
        if (!memoryPool_) {
            memoryPool_ = &utils::GlobalMemoryPool::getInstance();
        }
        // 从传统模式切换到内存池模式
        if (!buffer_.empty()) {
            if (!requestBuffer_) {
                requestBuffer_ = std::make_unique<utils::PooledBuffer>(memoryPool_);
            }
            requestBuffer_->write(buffer_);
            buffer_.clear();
        }
        
        if (!responseData_.empty()) {
            if (!responseBuffer_) {
                responseBuffer_ = std::make_unique<utils::PooledBuffer>(memoryPool_);
            }
            size_t written = responseBuffer_->write(responseData_);
            if (written < responseData_.size()) {
                // 装不下：保留 std::string 路径，不丢数据
                responseBuffer_.reset();
                truncated_ = true;
            } else {
                responseData_.clear();
            }
        }
    } else {
        // 从内存池模式切换到传统模式：先搬内容再析构池块
        if (requestBuffer_) {
            buffer_ = requestBuffer_->readString(requestBuffer_->getUsedSize());
            requestBuffer_.reset();
        }
        
        if (responseBuffer_) {
            // 池路径与 std::string 路径互斥，但搬移时以"已写长度"为准
            responseData_ = responseBuffer_->readString(responseBuffer_->getUsedSize());
            responseBuffer_.reset();
        }
    }
}

void HttpContext::printMemoryPoolStats() const {
    std::cout << "HttpContext Memory Pool Stats:" << std::endl;
    std::cout << "  Memory Pool Enabled: " << (useMemoryPool_ ? "Yes" : "No") << std::endl;
    std::cout << "  Request Buffer Size: " << (useMemoryPool_ && requestBuffer_ ? 
        requestBuffer_->getUsedSize() : buffer_.size()) << " bytes" << std::endl;
    std::cout << "  Response Buffer Size: " << (useMemoryPool_ && responseBuffer_ ? 
        responseBuffer_->getUsedSize() : responseData_.size()) << " bytes" << std::endl;
    
    if (useMemoryPool_) {
        std::cout << "  Pool Total Blocks: " << memoryPool_->getTotalBlocks() << std::endl;
        std::cout << "  Pool Used Blocks: " << memoryPool_->getUsedBlocks() << std::endl;
        std::cout << "  Pool Available Blocks: " << memoryPool_->getAvailableBlocks() << std::endl;
    }
}

} // namespace http
