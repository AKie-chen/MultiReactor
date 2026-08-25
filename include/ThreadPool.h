#pragma once
#include "EventLoop.h"
#include <functional>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>

// one-loop-per-thread worker 池：N 个 worker 线程各自运行一个独立 EventLoop，
// 无共享队列、无 mutex/condvar。tryRun = RR 分发 + queueInLoop（eventfd 唤醒）。
// 生命周期：loops_ 由 ThreadPool 独占持有，stop() 只 quit+join，loops_ 存活到
// 析构——main.cpp 拆除顺序注释中的不变式（stop 后 IO 线程仍可能 tryRun，
// 必须落进存活队列，永不执行但无 UAF）。注意：stop() 须在 worker 进入 loop()
// 之后调用（quit 会被 loop() 开头的 looping_=true 覆盖导致 join 挂起；
// 生产时序天然满足，单测需先做一次任务往返暖机）。
class ThreadPool {
public:
    using Task = std::function<void()>;

    ThreadPool(size_t numThreads, size_t maxQueueSize = 0); // 0 = 无上限
    ~ThreadPool();

    bool tryRun(Task task); // 非阻塞提交；在途（排队+执行）达上限返回 false → 503
    size_t queueSize() const; // 兼容保留：返回在途任务数（排队+执行中）
    void stop(); // quit 所有 loop + join（可重复调用）；排队未执行的任务被丢弃
    size_t inFlight() const { return inFlight_.load(std::memory_order_relaxed); }

private:
    void workerFunc(size_t idx); // 跑 loops_[idx]->loop()，quit 后自然返回

    std::vector<std::unique_ptr<EventLoop>> loops_; // 先声明先构造：线程启动前建好
    std::vector<std::thread> threads_;
    std::atomic<size_t> inFlight_{0}; // 已受理未完成（排队+执行中），兼作有界判据
    std::atomic<size_t> next_{0};     // RR 分发计数；tryRun 可被多个 IO 线程并发调用，必须 atomic
    size_t maxQueueSize_;             // 在途上限，0 = 无上限
};
