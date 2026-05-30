// test_infra_threadpool.cpp — ThreadPool 单元测试
// 验证：enqueue→future.get() 拿到结果、getQueueSize() 正确、
//       shutdown() 后 enqueue 抛异常、isRunning() 状态、waitForAllTasks() 阻塞

#include "utils/ThreadPool.h"
#include <iostream>
#include <chrono>
#include <atomic>
#include <vector>
#include <string>

#define TEST(name) std::cout << "  [测试] " << name << "... "
#define PASS() std::cout << "通过" << std::endl
#define FAIL(msg) do { std::cerr << "失败: " << msg << std::endl; return false; } while(0)
#define CHECK(cond, msg) if (!(cond)) FAIL(msg)

static int g_testsPassed = 0;
static int g_testsFailed = 0;

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

int main() {
    std::cout << "=== test_infra_threadpool ===" << std::endl;

    auto run = [](bool (*fn)(), const char* name) {
        std::cout << "[运行] " << name << std::endl;
        if (fn()) {
            g_testsPassed++;
        } else {
            g_testsFailed++;
        }
    };

    run(test_enqueue_and_result,     "enqueue 提交并获取结果");
    run(test_multiple_enqueue,       "批量任务提交");
    run(test_queue_size,             "队列大小");
    run(test_shutdown_throws,        "关闭后提交抛异常");
    run(test_is_running,             "运行状态");
    run(test_wait_for_all_tasks,     "等待全部任务完成");
    run(test_get_thread_count,       "获取线程数");

    std::cout << std::endl
              << "结果: " << g_testsPassed << " 通过, "
              << g_testsFailed << " 失败" << std::endl;

    return g_testsFailed > 0 ? 1 : 0;
}
