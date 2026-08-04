#pragma once
#include <functional>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

class ThreadPool {
public:
    using Task = std::function<void()>;

    ThreadPool(size_t numThreads, size_t maxQueueSize = 0); // 0 = 无上限
    ~ThreadPool();

    bool tryRun(Task task); // 提交任务，非阻塞
    size_t queueSize() const; // 获取当前任务队列大小

private:
    void workerLoop(); //每个工作的线程
    std::vector<std::thread> threads_;// 线程池中的线程
    std::queue<Task> tasks_;    // 任务队列
    std::mutex mutex_;  // 互斥锁，用于保护任务队列
    std::condition_variable cond_; // 条件变量，用于线程间的通知
    std::atomic<bool> running_{true};
    size_t maxQueueSize_; // 任务队列最大长度，0 = 无上限
};