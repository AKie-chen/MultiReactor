#include "TimerQueue.h"
#include "EventLoop.h"
#include <time.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <cassert>

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      timerChannel_(
          timerfd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC),
          loop_)
{
    timerChannel_.setReadCallback([this]() {
        handleRead();
    });
    timerChannel_.enableReading();
}

TimerQueue::~TimerQueue()
{
    close(timerfd_);
}

int64_t TimerQueue::addTimer(Timer::TimerCallback cb, int64_t expiration, double interval)
{
    // TimerQueue 与 EventLoop 绑定，必须在所属 IO 线程调用
    assert(loop_->isInLoopThread());

    // 检查是否需要更新 timerfd 的触发时间
    bool earliestChanged = (timerHeap_.empty() || expiration < timerHeap_.front().expiration);

    Timer timer(std::move(cb), expiration, interval);
    timer.setId(nextTimerId_++);
    timerHeap_.push_back({expiration, timer.id()});

    id2index_[timer.id()] = timerHeap_.size() - 1;
    siftUp(timerHeap_.size() - 1);
    id2timer_.emplace(timer.id(), std::move(timer));
    
    if (earliestChanged) resetTimerfd(expiration);

    return timer.id();
}

void TimerQueue::cancel(int64_t timerId)
{
    assert(loop_->isInLoopThread());

    auto it = id2index_.find(timerId);
    if (it == id2index_.end()) return;

    // 注意：eraseEntry 内部已按 key 清除该 id 的 id2index_ 条目（pop 掉谁就删谁），
    // 这里不能再 erase(it)—— it 已失效，解引用已释放节点是 UB（core 复现实测崩溃）
    eraseEntry(it->second);
    id2timer_.erase(timerId);
}

void TimerQueue::handleRead()
{
    uint64_t exp;
    read(timerfd_, &exp, sizeof(exp));

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = ts.tv_sec * 1'000'000 + ts.tv_nsec / 1'000;

    // 第一步：从堆里拿出所有到期的 timer，同时清理 id2index_
    std::vector<Timer> expired;
    while(!timerHeap_.empty() && timerHeap_.front().expiration <= now) {
        // 必须先拷贝 id 再用：eraseEntry(0) 会 swap + pop_back 改动 vector，
        // front() 的引用在调用后悬垂（指向被换到 0 号位的另一个元素），
        // 拿悬垂引用读 timerId 会误删/误弹别的定时器（core 复现实测）
        int64_t expiredId = timerHeap_.front().timerId;
        eraseEntry(0);              // 弹出对顶
        id2index_.erase(expiredId);
        auto it = id2timer_.find(expiredId);
        if (it == id2timer_.end()) continue; // 防御：id 失联时跳过，不再解引用 end()
        expired.push_back(std::move(it->second));
        id2timer_.erase(expiredId);
    }

    // 第二步：执行回调（此时 cancel 扫不到它们，安全）
    for (auto& timer : expired) {
        timer.run();
        if (timer.repeat()) {
            timer.restart(now);
            // 复用原 id 重新入堆（同 addTimer 的入堆逻辑，但不再分配新 id）
            timerHeap_.push_back({timer.expiration(), timer.id()});
            id2index_[timer.id()] = timerHeap_.size() - 1;
            siftUp(timerHeap_.size() - 1);
            id2timer_.emplace(timer.id(), std::move(timer));
        }
    }

    if (!timerHeap_.empty()) resetTimerfd(timerHeap_.front().expiration);
}

void TimerQueue::resetTimerfd(int64_t earliestExpiration)
{
    struct itimerspec newValue = {};

    int64_t microSec = earliestExpiration;

    newValue.it_value.tv_sec  = microSec / 1'000'000;
    newValue.it_value.tv_nsec = (microSec % 1'000'000) * 1'000;

    timerfd_settime(timerfd_, TFD_TIMER_ABSTIME, &newValue, nullptr);
}

void TimerQueue::siftUp(size_t index)
{
    // 0-based 堆：父节点 = (i-1)/2（原实现用 i/2 是 1-based 关系式，
    // 与 0-based vector 混用导致下标 2 的元素与错误的"父"比较，堆序错乱）
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (timerHeap_[index].expiration >= timerHeap_[parent].expiration) break;
        std::swap(timerHeap_[index], timerHeap_[parent]);
        id2index_[timerHeap_[index].timerId] = index;
        id2index_[timerHeap_[parent].timerId] = parent;
        index = parent;
    }
}

void TimerQueue::siftDown(size_t index)
{
    // 0-based 堆：左孩子 = 2i+1，右孩子 = 2i+2（原实现用 2i/2i+1 是 1-based 关系式）
    for (;;) {
        size_t left = index * 2 + 1;
        if (left >= timerHeap_.size()) return;   // 无孩子

        // 选出两个子中更早到期的（右孩子可能不存在）
        size_t smallest = left;
        size_t right = left + 1;
        if (right < timerHeap_.size() &&
            timerHeap_[right].expiration < timerHeap_[left].expiration) {
            smallest = right;
        }

        if (timerHeap_[index].expiration <= timerHeap_[smallest].expiration) return;
        std::swap(timerHeap_[index], timerHeap_[smallest]);
        id2index_[timerHeap_[index].timerId] = index;
        id2index_[timerHeap_[smallest].timerId] = smallest;
        index = smallest;
    }
}

void TimerQueue::eraseEntry(size_t index)
{
    if (index >= timerHeap_.size()) return;
    size_t lastIndex = timerHeap_.size() - 1;

    std::swap(timerHeap_[index], timerHeap_[lastIndex]);
    id2index_[timerHeap_[index].timerId] = index;
    id2index_.erase(timerHeap_[lastIndex].timerId);
    timerHeap_.pop_back();

    if(index < lastIndex) {
        siftUp(index);
        siftDown(index);
    }
}