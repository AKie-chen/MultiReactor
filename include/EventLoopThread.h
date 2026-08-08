#pragma once
#include "EventLoop.h"
#include <thread>
#include <mutex>
#include <condition_variable>

class EventLoopThread {
public:
    EventLoopThread();
    ~EventLoopThread();

    EventLoop* getLoop() const;   // 返回 loop 指针

private:
    void threadFunc();            // 线程函数：创建 loop → loop.loop()

    EventLoop* loop_;             // 堆上分配，线程析构时 delete
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
};