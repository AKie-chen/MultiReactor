#include "ThreadPool.h"

ThreadPool::ThreadPool(size_t numThreads, size_t maxQueueSize)
    : maxQueueSize_(maxQueueSize)
{
    for(size_t i = 0; i < numThreads; i++){
        threads_.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool()
{
    {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    }
    cond_.notify_all();
    for (auto& t : threads_) t.join();

}

bool ThreadPool::tryRun(Task task)//提交任务，非阻塞
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (maxQueueSize_ > 0 && tasks_.size() >= maxQueueSize_) {
            return false; // 队列已满
        }
        tasks_.push(std::move(task));
    }
    cond_.notify_one(); // 唤醒一个等待的线程
    return true;
}

size_t ThreadPool::queueSize() const // 获取当前任务队列大小
{
    return tasks_.size();
}

void ThreadPool::workerLoop() //每个工作的线程
{
    while(running_){
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this] { return !tasks_.empty() || !running_;});
            if (!running_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();  // 在锁外执行，不阻塞其他线程取任务
    }
}