// test_infra_threadpool.cpp — ThreadPool 单元测试
// 验证：enqueue→future.get() 拿到结果、getQueueSize() 正确、
//       shutdown() 后 enqueue 抛异常、isRunning() 状态、waitForAllTasks() 阻塞

#include "utils/ThreadPool.h"
#include <iostream>
#include <chrono>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <stdexcept>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)
// SKIP：报告为"跳过"（只计入 g_testsSkipped，并置 g_skipFlag 让 run() 不要把它
//       算成通过；返回值是 false，因此退出码语义不变 —— 跳过不算失败）
#define SKIP(msg) do { std::cout << "跳过 (" << msg << ")" << std::endl; ++g_testsSkipped; g_skipFlag = true; return false; } while(0)

static int g_testsPassed = 0;
static int g_testsFailed = 0;
static int g_testsSkipped = 0;
// SKIP 用：让 run() 知道"这次返回 false 是跳过，不是失败"
[[maybe_unused]] static bool g_skipFlag = false;

static bool test_enqueue_and_result() {
    TEST("enqueue 提交任务并返回正确结果");
    utils::ThreadPool pool(2);

    auto fut = pool.enqueue([](int a, int b) -> int {
        return a + b;
    }, 3, 4);

    int result = fut.get();
    CHECK(result == 7, "期望 7, 实际 " << result);

    pool.shutdown();
    PASS();
    return true;
}

static bool test_multiple_enqueue() {
    TEST("100 个任务全部正确完成");
    utils::ThreadPool pool(4);

    const int N = 100;
    std::vector<std::future<int>> futures;
    for (int i = 0; i < N; ++i) {
        futures.push_back(pool.enqueue([i]() -> int {
            return i * i;
        }));
    }

    for (int i = 0; i < N; ++i) {
        int result = futures[i].get();
        CHECK(result == i * i, "任务 " << i << " 期望 " << i*i << ", 实际 " << result);
    }

    pool.shutdown();
    PASS();
    return true;
}

static bool test_queue_size() {
    TEST("getQueueSize 反映真实积压数");
    utils::ThreadPool pool(1);
    // 先提交一个长时间任务阻塞唯一的工作线程
    std::atomic<bool> started{false};
    auto slow = pool.enqueue([&started]() {
        started.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return 0;
    });

    // 等待慢任务开始执行
    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // 此时工作线程被占用，再提交的任务应该排队
    auto fut2 = pool.enqueue([]() { return 42; });
    auto fut3 = pool.enqueue([]() { return 43; });

    // 队列中至少应该有 1-2 个任务（取决于调度时机）
    size_t qs = pool.getQueueSize();
    CHECK(qs >= 1, "期望队列大小 >= 1, 实际 " << qs);

    slow.get();
    fut2.get();
    fut3.get();

    pool.shutdown();
    PASS();
    return true;
}

static bool test_shutdown_throws() {
    TEST("shutdown 后 enqueue 抛出异常");
    utils::ThreadPool pool(2);
    pool.shutdown();

    bool threw = false;
    try {
        pool.enqueue([]() { return 1; });
    } catch (const std::runtime_error&) {
        threw = true;
    }

    CHECK(threw, "shutdown 后 enqueue 应抛出 runtime_error");

    PASS();
    return true;
}

static bool test_is_running() {
    TEST("isRunning 正确反映线程池状态");
    utils::ThreadPool pool(2);
    CHECK(pool.isRunning(), "构造后应处于运行状态");

    pool.shutdown();
    CHECK(!pool.isRunning(), "shutdown 后应停止运行");

    PASS();
    return true;
}

static bool test_wait_for_all_tasks() {
    TEST("waitForAllTasks 阻塞直到全部任务完成");
    utils::ThreadPool pool(4);

    std::atomic<int> counter{0};
    const int N = 50;

    for (int i = 0; i < N; ++i) {
        pool.enqueue([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            counter.fetch_add(1);
        });
    }

    pool.waitForAllTasks();
    CHECK(counter.load() == N, "期望 " << N << " 个任务完成, 实际 " << counter.load());

    pool.shutdown();
    PASS();
    return true;
}

static bool test_get_thread_count() {
    TEST("getThreadCount 返回正确的线程数");
    utils::ThreadPool pool(4);
    CHECK(pool.getThreadCount() == 4, "期望 4 线程, 实际 " << pool.getThreadCount());
    pool.shutdown();

    utils::ThreadPool pool2(8);
    CHECK(pool2.getThreadCount() == 8, "期望 8 线程, 实际 " << pool2.getThreadCount());
    pool2.shutdown();

    PASS();
    return true;
}

static bool test_enqueue_detached() {
    TEST("enqueueDetached 提交任务且不返回 future");
    utils::ThreadPool pool(2);

    std::atomic<int> counter{0};
    const int N = 50;
    for (int i = 0; i < N; ++i) {
        pool.enqueueDetached([&counter]() { counter.fetch_add(1); });
    }
    pool.waitForAllTasks();
    CHECK(counter.load() == N, "期望 " << N << " 个任务完成, 实际 " << counter.load());

    // 任务体内的异常不能逃逸（由 worker 捕获），且不影响后续任务
    std::atomic<int> after{0};
    pool.enqueueDetached([]() { throw std::runtime_error("boom"); });
    pool.enqueueDetached([&after]() { after.fetch_add(1); });
    pool.waitForAllTasks();
    CHECK(after.load() == 1, "异常任务之后的任务应照常执行, 实际 " << after.load());

    // 关闭后与 enqueue 同语义：抛 runtime_error
    pool.shutdown();
    bool threw = false;
    try {
        pool.enqueueDetached([]() {});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw, "shutdown 后 enqueueDetached 应抛出 runtime_error");

    PASS();
    return true;
}

// T2：空 std::function 必须被拒绝 —— worker 对空任务既不执行也不递减
// activeTasks_，放进去会让计数永不清零、waitForAllTasks() 永久阻塞。
// 判别力：把 enqueueDetached 的 !task 检查摘掉，本用例的 waitForAllTasks()
// 会卡在 3s 看门狗上（实机验证：watchdog _exit(42)）。
static bool test_enqueue_detached_rejects_empty() {
    TEST("enqueueDetached 空任务被拒且 waitForAllTasks 仍能收敛");
    utils::ThreadPool pool(2);

    std::atomic<int> ran{0};
    pool.enqueueDetached([&ran]() { ran.fetch_add(1); });

    std::function<void()> empty;
    bool threw = false;
    try {
        pool.enqueueDetached(empty);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw, "空 std::function 应抛 std::invalid_argument");
    // 队列里只应剩那 1 个正常任务（不能因为被拒的任务而变多）。
    // 注意不能直接断言 ==1 后立刻读：worker 随时会把任务取走，这里等它被取走。
    std::size_t qsz = pool.getQueueSize();
    for (int i = 0; i < 50 && qsz > 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        qsz = pool.getQueueSize();
    }
    CHECK(qsz <= 1, "被拒的任务不应进入队列, 实际队列长度 " << qsz);

    // 看门狗：3s 还没等到计数归零即判失败（不能真挂死测试进程）
    std::atomic<bool> done{false};
    std::thread watchdog([&done]() {
        for (int i = 0; i < 30 && !done.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!done.load()) {
            std::cerr << "\n[看门狗] waitForAllTasks 3s 未返回 → activeTasks_ 记账泄漏 " << std::flush;
            std::_Exit(97);
        }
    });
    pool.waitForAllTasks();
    done.store(true);
    watchdog.join();

    CHECK(ran.load() == 1, "正常任务应执行一次, 实际 " << ran.load());
    pool.shutdown();
    PASS();
    return true;
}

int main() {
    std::cout << "=== test_infra_threadpool ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        g_skipFlag = false;   // fn() 里的 SKIP 会置位
        if (fn()) { if (!g_skipFlag) ++g_testsPassed; }
        else if (!g_skipFlag) { ++g_testsFailed; }
    };

    run(test_enqueue_and_result,     "enqueue 提交并获取结果");
    run(test_multiple_enqueue,       "批量任务提交");
    run(test_queue_size,             "队列大小");
    run(test_shutdown_throws,        "关闭后提交抛异常");
    run(test_is_running,             "运行状态");
    run(test_wait_for_all_tasks,     "等待全部任务完成");
    run(test_get_thread_count,       "获取线程数");
    run(test_enqueue_detached,       "enqueueDetached 提交");
    run(test_enqueue_detached_rejects_empty, "enqueueDetached 空任务");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败, "
              << g_testsSkipped << " 跳过（总用例 "
              << (g_testsPassed + g_testsFailed + g_testsSkipped) << "）" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
