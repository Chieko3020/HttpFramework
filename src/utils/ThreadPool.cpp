#include "utils/ThreadPool.h"
#include <iostream>

namespace utils {

ThreadPool::ThreadPool(size_t threadCount) 
    : running_(true), activeTasks_(0) {
    
    // 创建工作线程
    for (size_t i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this] {
            workerFunction();
        });
    }
    
    std::cout << "[INFO][线程池]：初始化完成, 线程数=" << threadCount << "" << std::endl;
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::workerFunction() {
    while (running_.load()) {
        std::function<void()> task;
        
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            
            // 等待任务或关闭信号
            condition_.wait(lock, [this] {
                return !tasks_.empty() || !running_.load();
            });
            
            // 如果线程池关闭且没有任务，退出
            if (!running_.load() && tasks_.empty()) {
                break;
            }
            
            // 获取任务
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
            }
        }
        
        // 执行任务
        if (task) {
            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "[ERROR][线程池]：任务执行异常: " << e.what() << std::endl;
            }
            
            // 任务完成，减少活跃任务计数
            size_t remaining = activeTasks_.fetch_sub(1) - 1;
            if (remaining == 0) {
                finishedCondition_.notify_all();
            }
        }
    }
}

size_t ThreadPool::getQueueSize() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return tasks_.size();
}

void ThreadPool::shutdown() {
    if (!running_.load()) {
        return;
    }
    
    running_.store(false);
    
    // 唤醒所有等待的线程
    condition_.notify_all();
    
    // 等待所有工作线程结束
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    
    workers_.clear();
    
    std::cout << "[INFO][线程池]：已关闭" << std::endl;
}

void ThreadPool::waitForAllTasks() {
    std::unique_lock<std::mutex> lock(queueMutex_);
    finishedCondition_.wait(lock, [this] {
        return tasks_.empty() && activeTasks_.load() == 0;
    });
}

} // namespace utils
