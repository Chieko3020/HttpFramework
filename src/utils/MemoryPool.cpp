#include "utils/MemoryPool.h"
#include <iostream>
#include <algorithm>

namespace utils {

HttpMemoryPool::HttpMemoryPool(size_t poolSize) 
    : usedBlocks_(0) {
    initializePool(poolSize);
}

HttpMemoryPool::~HttpMemoryPool() {
    // 智能指针自动清理
}

void HttpMemoryPool::initializePool(size_t poolSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 预分配所有内存块
    blocks_.reserve(poolSize);
    for (size_t i = 0; i < poolSize; ++i) {
        auto block = std::make_unique<MemoryBlock>(i);
        freeBlocks_.push(block.get());
        blocks_.push_back(std::move(block));
    }
    
    std::cout << "[INFO][内存池]：初始化完成, 块数=" << poolSize 
              << " 块, 每块 " << BLOCK_SIZE << " 字节" << std::endl;
}

MemoryBlock* HttpMemoryPool::allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (freeBlocks_.empty()) {
        std::cerr << "[WARN][内存池]：无空闲块可用" << std::endl;
        return nullptr;
    }
    
    MemoryBlock* block = freeBlocks_.front();
    freeBlocks_.pop();
    block->isUsed = true;
    usedBlocks_++;
    
    return block;
}

void HttpMemoryPool::deallocate(MemoryBlock* block) {
    if (block == nullptr) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!block->isUsed) {
        std::cerr << "[WARN][内存池]：尝试释放未使用的块" << std::endl;
        return;
    }
    
    block->isUsed = false;
    freeBlocks_.push(block);
    usedBlocks_--;
}

void HttpMemoryPool::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 清空空闲队列
    while (!freeBlocks_.empty()) {
        freeBlocks_.pop();
    }
    
    // 重置所有块
    for (auto& block : blocks_) {
        block->isUsed = false;
        freeBlocks_.push(block.get());
    }
    
    usedBlocks_ = 0;
    std::cout << "[DEBUG][内存池]：已重置, 可用块数=" << blocks_.size() << " 块可用" << std::endl;
}

// PooledBuffer 实现
PooledBuffer::PooledBuffer(HttpMemoryPool* pool) 
    : pool_(pool), block_(nullptr), usedSize_(0) {
    block_ = pool_->allocate();
    if (block_) {
        std::memset(block_->data, 0, BLOCK_SIZE);
    }
}

PooledBuffer::~PooledBuffer() {
    if (block_) {
        pool_->deallocate(block_);
    }
}

PooledBuffer::PooledBuffer(PooledBuffer&& other) noexcept
    : pool_(other.pool_), block_(other.block_), usedSize_(other.usedSize_) {
    other.pool_ = nullptr;
    other.block_ = nullptr;
    other.usedSize_ = 0;
}

PooledBuffer& PooledBuffer::operator=(PooledBuffer&& other) noexcept {
    if (this != &other) {
        // 释放当前资源
        if (block_) {
            pool_->deallocate(block_);
        }
        
        // 移动资源
        pool_ = other.pool_;
        block_ = other.block_;
        usedSize_ = other.usedSize_;
        
        // 清空源对象
        other.pool_ = nullptr;
        other.block_ = nullptr;
        other.usedSize_ = 0;
    }
    return *this;
}

size_t PooledBuffer::write(const char* src, size_t len) {
    if (!block_ || !src) return 0;
    
    size_t availableSpace = BLOCK_SIZE - usedSize_;
    size_t bytesToWrite = std::min(len, availableSpace);
    
    std::memcpy(block_->data + usedSize_, src, bytesToWrite);
    usedSize_ += bytesToWrite;
    
    return bytesToWrite;
}

size_t PooledBuffer::write(const std::string& str) {
    return write(str.c_str(), str.length());
}

size_t PooledBuffer::read(char* dst, size_t len) {
    if (!block_ || !dst) return 0;
    
    size_t bytesToRead = std::min(len, usedSize_);
    std::memcpy(dst, block_->data, bytesToRead);
    
    return bytesToRead;
}

std::string PooledBuffer::readString(size_t len) {
    if (!block_) return "";
    
    size_t bytesToRead = std::min(len, usedSize_);
    return std::string(block_->data, bytesToRead);
}

void PooledBuffer::clear() {
    if (block_) {
        std::memset(block_->data, 0, BLOCK_SIZE);
        usedSize_ = 0;
    }
}

void PooledBuffer::consume(size_t n) {
    if (!block_ || n == 0) return;
    if (n >= usedSize_) {
        clear();
        return;
    }
    size_t remaining = usedSize_ - n;
    std::memmove(block_->data, block_->data + n, remaining);
    usedSize_ = remaining;
}

} // namespace utils
