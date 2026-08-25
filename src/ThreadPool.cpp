#include "ThreadPool.h"
#include "Log.h"

ThreadPool::ThreadPool(size_t numThreads, size_t maxQueueSize)
    : maxQueueSize_(maxQueueSize)
{
    // 先建 loop（池构造线程上创建，threadId_=构造线程；worker loop 的投递方
    // 恒为跨线程的 IO 线程，queueInLoop 恒走 eventfd 唤醒，threadId_ 无影响），
    // 再开线程：workerFunc 直接跑 loop()，无需任何发布同步
    for (size_t i = 0; i < numThreads; ++i) {
        loops_.push_back(std::make_unique<EventLoop>());
    }
    for (size_t i = 0; i < numThreads; ++i) {
        threads_.emplace_back([this, i] { workerFunc(i); });
    }
}

ThreadPool::~ThreadPool() { stop(); }

void ThreadPool::stop()
{
    // 全部 quit（eventfd 唤醒）：不先唤醒的话阻塞在 epoll_wait(5s) 的 worker
    // 会把 join 拖到超时
    for (auto& lp : loops_) lp->quit();
    for (auto& t : threads_) {
        if (t.joinable()) t.join(); // 可重复调用：已 join 过的线程不再 join
    }
    // join 后无任何线程再执行任务包装器：执行中的任务已在 join 期间自减完毕，
    // 排队未执行的任务从未自减 → 剩余值 = 被丢弃任务数，归零使 drainCheck 收敛。
    // 语义变化（vs 旧实现 stop 排空全部任务）：仅"最终排空之后"仍滞留的排队
    // 任务被丢弃——deadline 兜底场景下请求本已无法完成，可接受
    inFlight_.store(0, std::memory_order_relaxed);
}

bool ThreadPool::tryRun(Task task) // 提交任务，非阻塞
{
    if (loops_.empty()) return false; // workerThreads=0：诚实拒绝（503），旧行为是挂到 deadline

    // 有界判据 = 排队+执行中（比旧"仅队列长度"略紧，默认 1024 差异可忽略）。
    // 用 fetch_add 返回值而非"load 检查再 add"：并发下返回值唯一单调，杜绝
    // 检查-受理间隙的双重放行；拒绝时 fetch_sub 回滚
    size_t v = inFlight_.fetch_add(1, std::memory_order_relaxed);
    if (maxQueueSize_ > 0 && v >= maxQueueSize_) {
        inFlight_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    // RR 分发：next_ 必须 atomic（messageCallback 运行在多个 IO 线程上，
    // tryRun 并发调用；fetch_add 保证每个提交者拿到唯一序号，mod n 后互斥）
    size_t idx = next_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
    loops_[idx]->queueInLoop([this, task = std::move(task)]() mutable {
        // 异常隔离（最后一道防线）：业务 handler 的异常已被 main.cpp 的任务
        // 包装层转成 500，这里兜底保证任何逃逸的异常（含包装层自身的 bug）
        // 都不会杀死 worker 线程。inFlight_ 必须无条件自减——漏减会让在途
        // 计数虚高：后续请求误判背压（503），优雅关闭的 drainCheck 永不收敛
        try {
            task(); // 在 worker loop 线程执行
        } catch (const std::exception& e) {
            LOG_ERROR << "Exception in worker task: " << e.what();
        } catch (...) {
            LOG_ERROR << "Unknown exception in worker task";
        }
        inFlight_.fetch_sub(1, std::memory_order_relaxed);
    });
    return true;
}

size_t ThreadPool::queueSize() const
{
    return inFlight_.load(std::memory_order_relaxed);
}

void ThreadPool::workerFunc(size_t idx)
{
    loops_[idx]->loop(); // 阻塞直到 quit()；loop 由池所有，线程结束不释放
}
