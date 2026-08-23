#include "EventLoop.h"
#include "Channel.h"
#include "Log.h"
#include <fcntl.h>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/eventfd.h>

EventLoop::EventLoop()
    : looping_(false),
      epfd_(epoll_create1(0)),
      wakeupFd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      wakeupChannel_(wakeupFd_, this),
      timerQueue_(this) // 初始化定时器队列对象
{
    events_.resize(1024); // 初始化就绪事件数组，大小为1024
    wakeupChannel_.setReadCallback([this](){
        this->handleWakeup();
    });
    wakeupChannel_.enableReading(); // 使能唤醒Channel的可读事件
    threadId_ = std::this_thread::get_id(); // 获取当前线程的ID
}

EventLoop::~EventLoop() { close(epfd_); close(wakeupFd_); }

void EventLoop::loop() {
    looping_ = true;
    while(looping_){
        int react=epoll_wait(epfd_,events_.data(),events_.size(),5000);//等待事件发生，参数分别为epoll实例、就绪事件数组、最大事件数和超时时间（毫秒）
        
        if(react>0){//有fd可读
            // 动态扩容：如果 events_ 被填满，说明一次 epoll_wait 没有返回所有就绪 fd，
            // 下次可能还有更多，扩容以避免饥饿
            if (static_cast<size_t>(react) == events_.size()) {
                events_.resize(events_.size() * 2);
            }
            for(int i=0;i<react;i++){
                Channel* channel = static_cast<Channel*>(events_[i].data.ptr); // 获取就绪事件对应的Channel对象
                channel->handleEvent(events_[i].events); // 处理事件，根据事件类型调用相应的回调函数
            }
        }else if(react==0){
            LOG_DEBUG << "epoll_wait timeout, no events";
        }else if(react==-1){
            if(errno==EINTR){ continue; }// 被信号中断，继续等待事件
        }
        
        // 合并 eventfd 两次投递的接收侧：worker 投递用 wakeupPending_ 原子去重
        // （只有第一个投递者真正写 eventfd），这里把 pending 处理到彻底为空才解除武装
        // （wakeupPending_ = false）。必须 do-while 而不是单遍处理：
        // 单遍会在"处理期间新入队的任务"上丢唤醒——投递者看到 wakeupPending_ 仍置位
        // 会跳过 write，任务挂到下次 epoll 事件或 5s 超时才被处理。
        // 解除武装须在锁内且 pending 为空的时刻进行：此后入队的投递者 exchange
        // 拿到 false 才会自己写 eventfd，不会丢失
        for (;;) {
            std::vector<std::function<void()>> temp;
            {
                std::lock_guard<std::mutex> lock(mutex_); // 加锁，保护共享数据pendingFunctors_
                if (pendingFunctors_.empty()) {
                    wakeupPending_ = false; // 解除武装
                    break;
                }
                temp.swap(pendingFunctors_); // 交换任务，temp 拿到要执行的任务，pendingFunctors_ 清空等待新任务
            }
            callingPendingFunctors_ = true;
            for (auto& func : temp) {
                func();
            }
            callingPendingFunctors_ = false;
        }
    }
}

void EventLoop::quit() {
    looping_ = false;
    wakeup(); // 唤醒事件循环，使其退出
}

void EventLoop::updateChannel(Channel* channel,int op) {
    epoll_event ev;
    ev.events = channel->events(); // 获取Channel对象关注的事件类型
    ev.data.ptr = channel; // 将Channel对象的指针存储在事件数据中，以便在事件发生时能够获取到对应的Channel对象
    epoll_ctl(epfd_, op, channel->fd(), &ev); // 更新Channel对象在epoll中的事件
}

void EventLoop::removeChannel(Channel* channel) {
    epoll_ctl(epfd_, EPOLL_CTL_DEL, channel->fd(), nullptr); // 从epoll中移除Channel对象
}

void EventLoop::queueInLoop(std::function<void()> cb)//将销毁动作放进销毁队列，销毁动作由回调函数cb实现
{
    {
        std::lock_guard<std::mutex> lock(mutex_); // 加锁，保护共享数据pendingFunctors_
        pendingFunctors_.push_back(std::move(cb)); // 将回调函数cb添加到销毁队列中
    }
    if((std::this_thread::get_id() != threadId_ || callingPendingFunctors_) && !wakeupPending_.exchange(true)){
        wakeup(); // 如果当前线程不是事件循环所在的线程，或者正在调用销毁队列中的回调函数，则唤醒事件循环
    }
}

void EventLoop::handleWakeup() {
    // 循环读到 EAGAIN：一次 EPOLLIN 可能对应多次 write（计数累加）。
    // 必须全部清空——残留计数会压制后续 EPOLLIN（ET 只在计数 0→1 的状态变化时
    // 触发），后续投递的唤醒会被吞掉（原实现只读一次，多 worker 并发投递丢唤醒）
    uint64_t one;
    while (read(wakeupFd_, &one, sizeof(one)) == sizeof(one)) {}
}

void EventLoop::wakeup() // 唤醒事件循环
{
    uint64_t val = 1;
    write(wakeupFd_, &val, sizeof(val)); // 向wakeupFd_写入一个8字节的值，用于唤醒事件循环
}

TimerQueue& EventLoop::timerQueue() //获取定时器队列对象
{
    return timerQueue_;
}