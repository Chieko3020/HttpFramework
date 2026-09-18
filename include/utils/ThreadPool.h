#pragma once

#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>
#include <atomic>

namespace utils {

class ThreadPool {
public:
    explicit ThreadPool(size_t threadCount = std::thread::hardware_concurrency());
    ~ThreadPool();

    // 禁用拷贝构造和赋值
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 添加任务到线程池
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type>;

    // 提交"不关心结果"的任务：不构造 std::packaged_task 与 std::future，
    // 因此省掉 packaged_task 控制块 + 结果存储 + future 共享状态。
    // 请求热路径上调用方普遍丢弃 enqueue 的返回值（HttpServer 的请求任务、
    // WssReactor 的消息分发），这条路径每次提交少 2 次堆分配。
    // 语义与 enqueue 一致：池已关闭时抛 std::runtime_error（调用方据此撤销记账）。
    void enqueueDetached(std::function<void()> task);

    // 获取线程池状态
    size_t getThreadCount() const { return workers_.size(); }
    size_t getQueueSize() const;
    bool isRunning() const { return running_.load(); }

    // 优雅关闭
    void shutdown();
    void waitForAllTasks();

private:
    // 工作线程
    std::vector<std::thread> workers_;
    
    // 任务队列
    std::queue<std::function<void()>> tasks_;
    
    // 同步原语
    mutable std::mutex queueMutex_;
    std::condition_variable condition_;
    std::condition_variable finishedCondition_;
    
    // 状态控制
    std::atomic<bool> running_;
    std::atomic<size_t> activeTasks_;
    
    // 工作线程函数
    void workerFunction();
};

// 模板函数实现
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) 
    -> std::future<typename std::result_of<F(Args...)>::type> {
    
    using return_type = typename std::result_of<F(Args...)>::type;

    // 创建packaged_task
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> result = task->get_future();
    
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        
        // 检查线程池是否已关闭
        if (!running_.load()) {
            throw std::runtime_error("enqueue on stopped ThreadPool");
        }
        
        // 添加任务到队列
        tasks_.emplace([task](){ (*task)(); });
        activeTasks_.fetch_add(1);
    }
    
    // 通知一个等待的线程
    condition_.notify_one();
    return result;
}

} // namespace utils
