#pragma once
#include "Channel.h"
#include "TimerQueue.h"
#include <vector>
#include <sys/epoll.h>
#include <functional>
#include <mutex>
#include <thread>

class Channel;

class EventLoop {
private:
    int epfd_; // epoll实例的文件描述符
    bool looping_; // 事件循环是否正在运行
    bool quit_; // 退出事件循环的标志
    int wakeupFd_; // 用于唤醒事件循环的文件描述符
    Channel wakeupChannel_; // 用于唤醒事件循环的Channel对象
    std::mutex mutex_; // 互斥锁，用于保护共享数据
    std::thread::id threadId_; // 事件循环所在的线程ID
    std::vector<epoll_event> events_; // 就绪事件数组
    std::vector<std::function<void()>> pendingFunctors_;//销毁Channel指针的队列
    TimerQueue timerQueue_; // 定时器队列对象，用于管理定时器事件
    bool callingPendingFunctors_ = false; //判断是否进入销毁队列

    
public:
    EventLoop();
    ~EventLoop();
    
    void loop();
    void quit();
    
    void updateChannel(Channel* channel,int op); // 更新Channel对象在epoll中的事件
    void removeChannel(Channel* channel); // 从epoll中移除Channel对象
    void queueInLoop(std::function<void()> cb);//进入销毁队列的回调函数
    void handleWakeup(); // 处理唤醒事件
    void wakeup(); // 唤醒事件循环
    TimerQueue& timerQueue(); //获取定时器队列对象
    bool isInLoopThread() const { return std::this_thread::get_id() == threadId_; }
};