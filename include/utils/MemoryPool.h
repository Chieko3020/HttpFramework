#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <memory>
#include <cstring>

namespace utils {

// 内存块结构
struct MemoryBlock {
    char data[12288];  // 12KB数据块
    bool isUsed;
    size_t blockId;
    
    MemoryBlock(size_t id) : isUsed(false), blockId(id) {}
};

// HTTP专用内存池
class HttpMemoryPool {
public:
    static constexpr size_t BLOCK_SIZE = 12288;  // 12KB块大小，适合HTTP请求
    static constexpr size_t DEFAULT_POOL_SIZE = 5000;  // 默认5000个块，支持高并发
    
    HttpMemoryPool(size_t poolSize = DEFAULT_POOL_SIZE);
    ~HttpMemoryPool();
    
    // 分配内存块
    MemoryBlock* allocate();
    
    // 释放内存块
    void deallocate(MemoryBlock* block);
    
    // 获取统计信息
    size_t getTotalBlocks() const { return blocks_.size(); }
    size_t getUsedBlocks() const { return usedBlocks_; }
    size_t getAvailableBlocks() const { return getTotalBlocks() - getUsedBlocks(); }
    
    // 重置池（清空所有块）
    void reset();
    
private:
    std::vector<std::unique_ptr<MemoryBlock>> blocks_;
    std::queue<MemoryBlock*> freeBlocks_;
    std::mutex mutex_;
    size_t usedBlocks_;
    
    void initializePool(size_t poolSize);
};

// 缓冲区包装器
class PooledBuffer {
public:
    explicit PooledBuffer(HttpMemoryPool* pool);
    ~PooledBuffer();
    
    // 禁用拷贝，允许移动
    PooledBuffer(const PooledBuffer&) = delete;
    PooledBuffer& operator=(const PooledBuffer&) = delete;
    PooledBuffer(PooledBuffer&& other) noexcept;
    PooledBuffer& operator=(PooledBuffer&& other) noexcept;
    
    // 数据操作
    char* data() { return block_ ? block_->data : nullptr; }
    const char* data() const { return block_ ? block_->data : nullptr; }
    size_t size() const { return BLOCK_SIZE; }
    
    // 写入数据
    size_t write(const char* src, size_t len);
    size_t write(const std::string& str);
    
    // 读取数据
    size_t read(char* dst, size_t len);
    std::string readString(size_t len);
    
    // 清空缓冲区
    void clear();
    
    // 消费前 n 字节（将剩余数据前移，解决粘包问题）
    void consume(size_t n);
    
    // 获取已使用大小
    size_t getUsedSize() const { return usedSize_; }
    
private:
    HttpMemoryPool* pool_;
    MemoryBlock* block_;
    size_t usedSize_;
    
    static constexpr size_t BLOCK_SIZE = 12288;
};

// 全局内存池实例
class GlobalMemoryPool {
public:
    static HttpMemoryPool& getInstance() {
        static HttpMemoryPool instance;
        return instance;
    }
    
private:
    GlobalMemoryPool() = default;
};

} // namespace utils
