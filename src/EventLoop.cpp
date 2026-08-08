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
      quit_(false),
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
        }else{
            LOG_ERROR << "epoll_wait error: " << strerror(errno);
        }
        
        std::vector<std::function<void()>> temp; // 供销毁的vector
        {
            std::lock_guard<std::mutex> lock(mutex_); // 加锁，保护共享数据pendingFunctors_
            temp.swap(pendingFunctors_);//交换任务，temp拿到需要销毁的任务，而pendingFunction_则清空等待新的销毁任务
        }
        callingPendingFunctors_ = true;
        for (auto &func : temp) {
            func();
        }
        callingPendingFunctors_ = false;
    }
}

void EventLoop::quit() {
    quit_ = true;
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
    if(std::this_thread::get_id() != threadId_ || callingPendingFunctors_){
        wakeup(); // 如果当前线程不是事件循环所在的线程，或者正在调用销毁队列中的回调函数，则唤醒事件循环
    }
}

void EventLoop::handleWakeup() {
    uint64_t one;
    ssize_t n = read(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        LOG_ERROR << "handleWakeup() reads " << n << " bytes instead of 8";
    }
}

void EventLoop::wakeup() // 唤醒事件循环
{
    uint64_t val = 1;
    write(wakeupFd_, &val, sizeof(val));
}

TimerQueue& EventLoop::timerQueue() //获取定时器队列对象
{
    return timerQueue_;
}