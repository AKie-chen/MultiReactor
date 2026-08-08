#include "Timer.h"

Timer::Timer(TimerCallback cb, int64_t expiration, double interval)
    : callback_(std::move(cb)),
      expiration_(expiration),
      interval_(interval)
{}

// 返回定时器的绝对过期时间（微秒时间戳）
int64_t Timer::expiration() const
{
    return expiration_;
}

// 只有 interval_ > 0 才算重复定时器
bool Timer::repeat() const
{
    if (interval_ > 0) return true;
    return false;
}

// 执行用户注册的回调函数
void Timer::run() const
{
    callback_();
}

// 重复定时器到期后调用：把过期时间向前推 interval_ 秒
// now: 当前时刻（微秒），下次过期时间 = now + interval_秒
void Timer::restart(int64_t now)
{
    // interval_ 是秒，转成微秒加上去
    expiration_ = now + static_cast<int64_t>(interval_ * 1'000'000);
}