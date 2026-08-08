#pragma once
#include "Channel.h"
#include "Timer.h"
#include <map>
#include <vector>
#include <unordered_map>

class EventLoop;
// 定时器队列 — 管理一组 Timer，用 timerfd + epoll 统一调度
//
// 工作原理：
//   1. 内核 timerfd 在最近一个 Timer 的过期时间点变为可读
//   2. epoll 检测到 timerChannel_ 的 EPOLLIN → 触发 handleRead()
//   3. handleRead 遍历 timers_ map，执行所有到期的回调
//   4. 如果 map 里还有未到期的，resetTimerfd 设置下一次唤醒时间
//
// timers_ 的 key 是过期时间（微秒），value 是 Timer 对象
// begin() 返回的就是最早到期的 Timer
class TimerQueue {
public:
    TimerQueue(EventLoop* loop);
    ~TimerQueue();

    // 添加一个定时器，返回 timerId（暂时不用 cancel 的话可以忽略返回值）
    // expiration: 绝对过期时间（微秒，用 CLOCK_MONOTONIC）
    // interval: 重复间隔（秒），0 = 一次性
    int64_t addTimer(Timer::TimerCallback cb, int64_t expiration, double interval = 0.0);

    // 取消定时器
    void cancel(int64_t timerId);

private:
    // timerfd 变为可读时调用 — 收集到期 timer 并执行回调
    void handleRead();

    // 用 timerfd_settime 设置 timerfd 的下一次触发时间
    // earliestExpiration: 微秒时间戳，是 timers_.begin()->first
    void resetTimerfd(int64_t earliestExpiration);

    EventLoop* loop_;              // 所属事件循环
    int timerfd_;                  // timerfd 文件描述符（内核定时器）
    Channel timerChannel_;         // 用 Channel 包装 timerfd，交给 epoll 监听

    struct HeapEntry{
        int64_t expiration; // 过期时间（微秒）
        int64_t timerId;    // 定时器 ID
        bool operator<(const HeapEntry& other) const {
            return expiration != other.expiration ? expiration > other.expiration : timerId > other.timerId;
        }
    };

    std::vector<HeapEntry> timerHeap_; // 小顶堆，按 expiration 升序排列
    std::unordered_map<int64_t, size_t> id2index_; // timerId -> 堆下标
    std::unordered_map<int64_t, Timer> id2timer_; // timerId -> Timer 对象
    int64_t nextTimerId_ = 1; // 下一个定时器 ID（从 1 开始：0 被 main.cpp 用作"无定时器"哨兵）

    void siftUp(size_t index);
    void siftDown(size_t index);
    void eraseEntry(size_t index);
};
