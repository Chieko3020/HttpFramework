#include "utils/MemoryPool.h"
#include <iostream>
#include <algorithm>

namespace utils {

HttpMemoryPool::HttpMemoryPool(size_t poolSize, size_t blockSize)
    : blockSize_(0), usedBlocks_(0) {
    if (poolSize == 0) poolSize = DEFAULT_POOL_SIZE;
    if (blockSize < MIN_BLOCK_SIZE) blockSize = MIN_BLOCK_SIZE;
    if (blockSize > BLOCK_SIZE) blockSize = BLOCK_SIZE;
    blockSize_ = blockSize;
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
        auto block = std::make_unique<MemoryBlock>(i, blockSize_);
        freeBlocks_.push(block.get());
        blocks_.push_back(std::move(block));
    }
    
    std::cout << "[INFO][内存池]：初始化完成, 块数=" << poolSize 
              << " 块, 每块 " << blockSize_ << " 字节" << std::endl;
}

MemoryBlock* HttpMemoryPool::allocate() {
    allocCalls_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (freeBlocks_.empty()) {
        // 池耗尽：显式计数并报错（调用方据此走 503/500，而不是静默"永远写不进"）（H8）
        uint64_t fails = allocateFailures_.fetch_add(1, std::memory_order_relaxed) + 1;
        // 只打印前若干次与之后每 1000 次，避免刷爆日志
        if (fails <= 5 || fails % 1000 == 0) {
            std::cerr << "[ERROR][内存池]：无空闲块可用（池耗尽）, 累计失败=" << fails
                      << " 块数=" << blocks_.size() << std::endl;
        }
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
    deallocCalls_.fetch_add(1, std::memory_order_relaxed);
    
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
    // 不在构造期整块清零（H8）：写入按 usedSize_ 定位、读取以 usedSize_ 为界，
    // 没有"读未清零区域"的路径；每连接一次 12KB memset 在高并发下是纯浪费。
    if (!block_) {
        std::cerr << "[ERROR][内存池]：缓冲区分配失败（池耗尽），该连接将无法写入数据"
                  << std::endl;
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
    
    size_t availableSpace = block_->capacity - usedSize_;
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
        // 仅重置已用长度：后续写入按 usedSize_ 定位、消费按 usedSize_ 判断边界，
        // 不依赖数据被清零。原先的 12KB memset 在长连接下每请求触发两次
        // （读完后 consume 全消费、响应发完后 resetForNextRequest），
        // 等于每请求 24KB 的无效内存写入，是高并发下的纯浪费。
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


// ── 全局内存池（H7：容量参数可配置，但只能在创建前设定）──

namespace {
std::mutex g_globalPoolMutex;
std::unique_ptr<HttpMemoryPool> g_globalPool;

HttpMemoryPool* globalPoolRaw() {
    std::lock_guard<std::mutex> lk(g_globalPoolMutex);
    return g_globalPool.get();
}
}  // namespace

HttpMemoryPool& GlobalMemoryPool::getInstance() {
    std::lock_guard<std::mutex> lk(g_globalPoolMutex);
    if (!g_globalPool) {
        g_globalPool = std::make_unique<HttpMemoryPool>();
    }
    return *g_globalPool;
}

bool GlobalMemoryPool::configure(size_t poolSize, size_t blockSize) {
    std::lock_guard<std::mutex> lk(g_globalPoolMutex);
    if (g_globalPool) {
        if (poolSize == 0 || blockSize == 0 ||
            (g_globalPool->getTotalBlocks() == poolSize &&
             g_globalPool->blockSize() == blockSize)) {
            return true;  // 参数相同：幂等
        }
        std::cerr << "[WARN][内存池]：内存池已创建，configure(poolSize=" << poolSize
                  << ", blockSize=" << blockSize
                  << ") 被忽略（必须在首次使用前配置）" << std::endl;
        return false;
    }
    g_globalPool = std::make_unique<HttpMemoryPool>(poolSize, blockSize);
    return true;
}

bool GlobalMemoryPool::isCreated() {
    return globalPoolRaw() != nullptr;
}

} // namespace utils
