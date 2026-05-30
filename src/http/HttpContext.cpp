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
    } else {
        buffer_.clear();
        responseData_.clear();
    }
    
    // 重置写入偏移量
    writeOffset_ = 0;
}

void HttpContext::appendData(const std::string& data) {
    if (useMemoryPool_) {
        if (!requestBuffer_) {
            requestBuffer_ = std::make_unique<utils::PooledBuffer>(memoryPool_);
        }
        requestBuffer_->write(data);
    } else {
        buffer_ += data;
    }
}

void HttpContext::appendData(const char* data, size_t len) {
    if (useMemoryPool_) {
        if (!requestBuffer_) {
            requestBuffer_ = std::make_unique<utils::PooledBuffer>(memoryPool_);
        }
        requestBuffer_->write(data, len);
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

void HttpContext::setResponseData(const std::string& data) {
    if (useMemoryPool_) {
        // 强制使用内存池，不再回退到std::string
        if (!responseBuffer_) {
            responseBuffer_ = std::make_unique<utils::PooledBuffer>(memoryPool_);
        }
        responseBuffer_->clear();
        responseBuffer_->write(data);
    } else {
        // 内存池未启用，使用传统方式
        responseData_ = data;
    }
}

void HttpContext::setResponseData(const char* data, size_t len) {
    if (useMemoryPool_) {
        // 强制使用内存池，不再回退到std::string
        if (!responseBuffer_) {
            responseBuffer_ = std::make_unique<utils::PooledBuffer>(memoryPool_);
        }
        responseBuffer_->clear();
        responseBuffer_->write(data, len);
    } else {
        // 内存池未启用，使用传统方式
        responseData_.assign(data, len);
    }
}

std::string HttpContext::getResponseData() const {
    if (useMemoryPool_) {
        if (responseBuffer_) {
            return responseBuffer_->readString(responseBuffer_->getUsedSize());
        }
        return {};
    } else {
        return responseData_;
    }
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
            responseBuffer_->write(responseData_);
            responseData_.clear();
        }
    } else {
        // 从内存池模式切换到传统模式
        if (requestBuffer_) {
            buffer_ = requestBuffer_->readString(requestBuffer_->getUsedSize());
            requestBuffer_.reset();
        }
        
        if (responseBuffer_) {
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

bool HttpContext::hasMoreDataToWrite() const {
    if (useMemoryPool_) {
        if (responseBuffer_) {
            return writeOffset_ < responseBuffer_->getUsedSize();
        }
        return false;
    } else {
        return writeOffset_ < responseData_.size();
    }
}

} // namespace http
