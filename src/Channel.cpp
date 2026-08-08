#include "Channel.h"
#include "EventLoop.h"
#include <sys/epoll.h>
#include <unistd.h>

Channel::Channel(int fd, EventLoop* loop) : fd_(fd), loop_(loop), events_(0), revents_(0) {}

Channel::~Channel() {
    if (added_) {
        loop_->removeChannel(this); // 从事件循环中移除当前Channel对象
        added_ = false;
    }
    close(fd_); // 关闭文件描述符，释放资源
}

void Channel::handleEvent(uint32_t events)// 处理事件，根据事件类型调用相应的回调函数
{
    if((events & EPOLLIN) && readCallback_){
        readCallback_();
    }
    if((events & EPOLLOUT) && writeCallback_){
        writeCallback_();
    }
    // EPOLLRDHUP: 对端关闭连接 (read half) — 视为错误，触发清理
    if((events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) && errorCallback_){
        errorCallback_();
    }
}

void Channel::enableReading() // 使能可读事件
{
    int op=added_?EPOLL_CTL_MOD:EPOLL_CTL_ADD;// 判断是添加还是修改事件
    events_ |= (EPOLLIN | EPOLLET); // 将EPOLLIN事件添加到关注的事件类型中
    data_.ptr = this; // 将Channel对象的指针存储在事件数据中，以便在事件发生时能够获取到对应的Channel对象
    loop_->updateChannel(this,op); // 更新Channel对象在epoll中的事件
    added_ = true; // 标志Channel对象已经添加到epoll中
}

void Channel::enableWriting() // 使能可写事件
{
    int op=added_?EPOLL_CTL_MOD:EPOLL_CTL_ADD;// 判断是添加还是修改事件
    events_ |= (EPOLLOUT | EPOLLET); // 将EPOLLOUT事件添加到关注的事件类型中
    data_.ptr = this; // 将Channel对象的指针存储在事件数据中，以便在事件发生时能够获取到对应的Channel对象
    loop_->updateChannel(this,op); // 更新Channel对象在epoll中的事件
    added_ = true; // 标志Channel对象已经添加到epoll中
}

void Channel::disableReading() // 禁止可读事件
{
    if(added_){
        events_ &= ~EPOLLIN; // 从关注的事件类型中移除EPOLLIN事件
        data_.ptr = this;   // 将Channel对象的指针存储在事件数据中，以便在事件发生时能够获取到对应的Channel对象
        loop_->updateChannel(this,EPOLL_CTL_MOD); // 更新Channel对象在epoll中的事件
    }
}

void Channel::disableWriting() // 禁止可写事件
{
    if(added_){
        events_ &= ~EPOLLOUT; // 从关注的事件类型中移除EPOLLOUT事件
        data_.ptr = this; // 将Channel对象的指针存储在事件数据中，以便在事件发生时能够获取到对应的Channel对象
        loop_->updateChannel(this,EPOLL_CTL_MOD); // 更新Channel对象在epoll中的事件
    }
}

void Channel::disableAll() // 移除所有事件监听
{
    if (added_) {
        loop_->removeChannel(this);
        added_ = false;
        events_ = 0;
    }
}
