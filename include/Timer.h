#pragma once
#include <functional>
#include <cstdint>

// 定时器对象 — 持有回调+过期时间+重复间隔
// 不感知 epoll/timerfd，纯数据+逻辑
class Timer {
public:
    using TimerCallback = std::function<void()>;

    // cb: 到期时执行的回调
    // expiration: 绝对过期时间，单位微秒（用 clock_gettime(CLOCK_MONOTONIC) 获取）
    // interval: 重复间隔，单位秒。0 表示一次性定时器，>0 表示每隔 interval 秒重复触发
    Timer(TimerCallback cb, int64_t expiration, double interval = 0.0);

    int64_t expiration() const;     // 返回绝对过期时间（微秒）
    int64_t id() const {return id_;}
    bool repeat() const;            // true = 重复定时器（interval > 0）
    void run() const;               // 执行用户回调
    void restart(int64_t now);      // 用当前时间 now 重新计算并设置下次过期时间
    void setId(int64_t id) {id_ = id;}

private:
    TimerCallback callback_;   // 用户注册的回调函数
    int64_t expiration_;       // 微秒级时间戳（从 CLOCK_MONOTONIC 获取）
    double interval_;          // 重复间隔（秒），0.0 = 一次性
    int id_;                   // 定时器序号
};
