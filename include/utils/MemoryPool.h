#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <memory>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace utils {

// 池内单块的容量上限（MemoryBlock::data 的固定数组长度）。
// 实际使用的块容量可由 HttpMemoryPool 的 blockSize 参数调小（H7）。
inline constexpr size_t kMaxHttpBlockSize = 12288;

// 内存块结构
struct MemoryBlock {
    char data[kMaxHttpBlockSize];  // 数据区上限 12KB
    size_t capacity;               // 本块实际可用容量（<= kMaxHttpBlockSize）
    bool isUsed;
    size_t blockId;

    MemoryBlock(size_t id, size_t cap = kMaxHttpBlockSize)
        : capacity(cap > kMaxHttpBlockSize ? kMaxHttpBlockSize : cap),
          isUsed(false), blockId(id) {}
};

// HTTP专用内存池
class HttpMemoryPool {
public:
    static constexpr size_t BLOCK_SIZE = kMaxHttpBlockSize;  // 默认单块容量
    static constexpr size_t MIN_BLOCK_SIZE = 1024;           // 允许的最小块容量
    static constexpr size_t DEFAULT_POOL_SIZE = 5000;        // 默认块数

    // blockSize 会被钳制到 [MIN_BLOCK_SIZE, BLOCK_SIZE]（H7：块容量可配置）
    HttpMemoryPool(size_t poolSize = DEFAULT_POOL_SIZE,
                   size_t blockSize = BLOCK_SIZE);
    ~HttpMemoryPool();

    // 分配内存块
    MemoryBlock* allocate();

    // 释放内存块
    void deallocate(MemoryBlock* block);

    // 本池的单块容量
    size_t blockSize() const { return blockSize_; }

    // 分配失败（池耗尽等）的次数：allocate() 返回 nullptr 不再是静默降级（H8）
    uint64_t allocateFailures() const {
        return allocateFailures_.load(std::memory_order_relaxed);
    }
    
    // 分配/释放调用次数：用于评估池在真实负载下的使用频率
    // （若远小于请求数，说明连接级 buffer 复用已经消除了分配压力）
    uint64_t allocCalls() const { return allocCalls_.load(std::memory_order_relaxed); }
    uint64_t deallocCalls() const { return deallocCalls_.load(std::memory_order_relaxed); }

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
    size_t blockSize_;
    size_t usedBlocks_;
    std::atomic<uint64_t> allocCalls_{0};
    std::atomic<uint64_t> deallocCalls_{0};
    std::atomic<uint64_t> allocateFailures_{0};

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
    
    // 是否真正持有一块内存（分配失败时为 false：此缓冲区"永远写不进"）
    bool valid() const { return block_ != nullptr; }

    // 数据操作
    char* data() { return block_ ? block_->data : nullptr; }
    const char* data() const { return block_ ? block_->data : nullptr; }
    // 原始只读指针（配合 usedSize() 做零拷贝读取）
    const char* rawData() const { return block_ ? block_->data : nullptr; }
    // 本缓冲区的容量：取自所持有内存块的实际容量（构造时由池给出）
    size_t size() const { return block_ ? block_->capacity : 0; }
    
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
};

// 全局内存池实例（进程级单例，惰性创建）
// configure() 只能在首次 getInstance() 之前调用：池一旦创建就无法改变容量参数
// （运行期改参数会让"新旧连接块大小不一致"，属于难以复现的差异）。
class GlobalMemoryPool {
public:
    static HttpMemoryPool& getInstance();
    static bool configure(size_t poolSize, size_t blockSize);
    static bool isCreated();
};

} // namespace utils
