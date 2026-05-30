// test_infra_mempool.cpp — HttpMemoryPool 单元测试
// 验证：allocate() 返回非空块、deallocate() 后块可复用、
//       池耗尽后 allocate() 返回 nullptr、reset() 后可用块数恢复、
//       PooledBuffer RAII 行为

#include "utils/MemoryPool.h"
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)

static int g_testsPassed = 0;
static int g_testsFailed = 0;

static bool test_allocate_non_null() {
    TEST("allocate 返回非空块且标记 isUsed");
    utils::HttpMemoryPool pool(10);

    auto* block = pool.allocate();
    CHECK(block != nullptr, "allocate() 应返回非空指针");
    CHECK(block->isUsed, "分配后 isUsed 应为 true");

    pool.deallocate(block);
    CHECK(!block->isUsed, "归还后 isUsed 应为 false");
    PASS();
    return true;
}

static bool test_deallocate_reuse() {
    TEST("归还的块按 FIFO 顺序复用");
    utils::HttpMemoryPool pool(10);

    // 分配所有 10 个块
    std::vector<utils::MemoryBlock*> blocks;
    for (int i = 0; i < 10; ++i) {
        auto* b = pool.allocate();
        CHECK(b != nullptr, "第 " << i << " 次分配应成功");
        blocks.push_back(b);
    }

    // 归还第 0 个块
    pool.deallocate(blocks[0]);
    CHECK(pool.getAvailableBlocks() == 1, "归还 1 个块后应有 1 个可用");

    // 再次分配，应该拿到刚刚归还的块（FIFO 队列头部）
    auto* reused = pool.allocate();
    CHECK(reused != nullptr, "归还后应能再次分配到块");
    CHECK(reused == blocks[0], "FIFO: 先归还的块应先被重新分配");

    // 清理
    pool.deallocate(reused);
    for (int i = 1; i < 10; ++i) pool.deallocate(blocks[i]);
    PASS();
    return true;
}

static bool test_exhaustion_returns_null() {
    TEST("池耗尽后 allocate 返回 nullptr");
    utils::HttpMemoryPool pool(5);

    std::vector<utils::MemoryBlock*> blocks;
    for (int i = 0; i < 5; ++i) {
        auto* b = pool.allocate();
        CHECK(b != nullptr, "第 " << i << " 次分配应成功");
        blocks.push_back(b);
    }

    // 第 6 次分配应返回 nullptr
    auto* extra = pool.allocate();
    CHECK(extra == nullptr, "池耗尽后 allocate 应返回 nullptr");

    for (auto* b : blocks) pool.deallocate(b);
    PASS();
    return true;
}

static bool test_reset() {
    TEST("reset 恢复所有可用块");
    utils::HttpMemoryPool pool(10);

    std::vector<utils::MemoryBlock*> blocks;
    for (int i = 0; i < 7; ++i) {
        blocks.push_back(pool.allocate());
    }
    CHECK(pool.getAvailableBlocks() == 3, "期望 3 可用, 实际 " << pool.getAvailableBlocks());
    CHECK(pool.getUsedBlocks() == 7, "期望 7 已用, 实际 " << pool.getUsedBlocks());

    pool.reset();
    CHECK(pool.getAvailableBlocks() == 10, "reset 后应有 10 可用");
    CHECK(pool.getUsedBlocks() == 0, "reset 后已用应为 0");

    auto* b = pool.allocate();
    CHECK(b != nullptr, "reset 后应能正常分配");
    pool.deallocate(b);

    PASS();
    return true;
}

static bool test_statistics() {
    TEST("统计信息实时准确");
    utils::HttpMemoryPool pool(20);

    CHECK(pool.getTotalBlocks() == 20, "总块数应为 20");
    CHECK(pool.getUsedBlocks() == 0, "初始已用应为 0");
    CHECK(pool.getAvailableBlocks() == 20, "初始可用应为 20");

    auto* b1 = pool.allocate();
    CHECK(pool.getUsedBlocks() == 1, "分配 1 个后已用应为 1");
    CHECK(pool.getAvailableBlocks() == 19, "分配 1 个后可用应为 19");

    auto* b2 = pool.allocate();
    CHECK(pool.getUsedBlocks() == 2, "分配 2 个后已用应为 2");

    pool.deallocate(b1);
    CHECK(pool.getUsedBlocks() == 1, "归还后已用应为 1");
    CHECK(pool.getAvailableBlocks() == 19, "归还后可用应为 19");

    pool.deallocate(b2);
    CHECK(pool.getUsedBlocks() == 0, "全部归还后已用应为 0");

    PASS();
    return true;
}

static bool test_double_dealloc_warning() {
    TEST("重复归还同一块不崩溃");
    utils::HttpMemoryPool pool(10);

    auto* b = pool.allocate();
    pool.deallocate(b);
    pool.deallocate(b);  // 应输出警告但不崩溃

    PASS();
    return true;
}

static bool test_pooled_buffer_raii() {
    TEST("PooledBuffer 构造时自动分配析构时自动归还");
    utils::HttpMemoryPool pool(10);

    CHECK(pool.getUsedBlocks() == 0, "创建前已用应为 0");

    {
        utils::PooledBuffer buf(&pool);
        CHECK(pool.getUsedBlocks() == 1, "存活期间已用应为 1");
        CHECK(buf.data() != nullptr, "buffer 数据指针不应为空");
        CHECK(buf.size() == 12288, "buffer 大小应为 12KB");
    }  // 析构

    CHECK(pool.getUsedBlocks() == 0, "析构后已用应为 0");

    PASS();
    return true;
}

static bool test_pooled_buffer_write_read() {
    TEST("PooledBuffer 读写操作");
    utils::HttpMemoryPool pool(10);
    utils::PooledBuffer buf(&pool);

    std::string testStr = "你好, 内存池!";
    size_t written = buf.write(testStr);
    CHECK(written == testStr.size(), "写入大小不匹配");

    std::string readBack = buf.readString(testStr.size());
    CHECK(readBack == testStr, "读回内容不匹配: '" << readBack << "'");

    buf.clear();
    CHECK(buf.getUsedSize() == 0, "清空后已用大小应为 0");

    PASS();
    return true;
}

static bool test_pooled_buffer_move() {
    TEST("PooledBuffer 移动语义正确转移所有权");
    utils::HttpMemoryPool pool(10);

    utils::PooledBuffer buf1(&pool);
    buf1.write("test data");

    CHECK(pool.getUsedBlocks() == 1, "buf1 存活期间已用应为 1");

    utils::PooledBuffer buf2(std::move(buf1));
    CHECK(pool.getUsedBlocks() == 1, "移动后已用仍为 1（所有权转移）");
    CHECK(buf2.getUsedSize() > 0, "buf2 应持有数据");

    PASS();
    return true;
}

static bool test_thread_safety() {
    TEST("多线程并发 alloc/dealloc 无损坏");
    utils::HttpMemoryPool pool(500);

    std::atomic<int> errors{0};

    auto worker = [&pool, &errors](int id) {
        for (int i = 0; i < 100; ++i) {
            auto* b = pool.allocate();
            if (b) {
                b->data[0] = static_cast<char>(id);
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                pool.deallocate(b);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) t.join();

    CHECK(pool.getUsedBlocks() == 0, "并发操作后已用应为 0, 实际 " << pool.getUsedBlocks());
    CHECK(errors.load() == 0, "并发操作中不应有错误");

    PASS();
    return true;
}

static bool test_global_memory_pool() {
    TEST("GlobalMemoryPool 单例可访问");
    utils::HttpMemoryPool& global = utils::GlobalMemoryPool::getInstance();
    auto* b = global.allocate();
    CHECK(b != nullptr, "全局池分配失败");
    global.deallocate(b);

    utils::HttpMemoryPool& global2 = utils::GlobalMemoryPool::getInstance();
    CHECK(&global == &global2, "GlobalMemoryPool 应为单例");

    PASS();
    return true;
}

int main() {
    std::cout << "=== test_infra_mempool ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) { g_testsPassed++; }
        else      { g_testsFailed++; }
    };

    run(test_allocate_non_null,       "分配非空块");
    run(test_deallocate_reuse,        "归还后 FIFO 复用");
    run(test_exhaustion_returns_null, "池耗尽返回 nullptr");
    run(test_reset,                   "重置");
    run(test_statistics,              "统计信息");
    run(test_double_dealloc_warning,  "重复归还");
    run(test_pooled_buffer_raii,      "PooledBuffer RAII");
    run(test_pooled_buffer_write_read,"PooledBuffer 读写");
    run(test_pooled_buffer_move,      "PooledBuffer 移动");
    run(test_thread_safety,           "线程安全");
    run(test_global_memory_pool,      "全局内存池");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
