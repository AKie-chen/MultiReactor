#include "EventLoopThread.h"
#include <pthread.h>
#include <memory>

EventLoopThread::EventLoopThread()
{
    thread_ = std::thread(&EventLoopThread::threadFunc, this); // 创建线程，执行线程函数
    std::unique_lock<std::mutex> lock(mutex_); // 加锁，保护共享数据loop_
    cond_.wait(lock, [this] { return loop_ != nullptr; }); // 等待条件变量，直到loop_被初始化
}

EventLoopThread::~EventLoopThread() {
    if (loop_) loop_->quit();        // 通知 IO 线程退出
    if (thread_.joinable()) thread_.join();
}

EventLoop* EventLoopThread::getLoop() const   // 返回 loop 指针
{
    return loop_;
}

void EventLoopThread::threadFunc()            // 线程函数：创建 loop → loop.loop()
{
    auto loop = std::make_unique<EventLoop>(); // 创建事件循环对象
    {
        std::lock_guard<std::mutex> lock(mutex_); // 加锁，保护共享数据loop_
        loop_ = loop.get(); // 将loop_指针指向事件循环对象
    }
    cond_.notify_one(); // 通知等待的线程，loop_已被初始化
    loop->loop(); // 进入事件循环，开始处理事件
}