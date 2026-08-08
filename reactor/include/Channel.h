#pragma once
#ifndef CHANNEL_H
#define CHANNEL_H

#include <sys/epoll.h>
#include <functional>
#include <cstdint>

class EventLoop;

class Channel {
private:
    int fd_; // 文件描述符
    EventLoop* loop_; // 事件循环对象的指针
    uint32_t events_; // 关注的事件类型
    uint32_t revents_; // 实际发生的事件类型
    epoll_data_t data_; // epoll事件数据结构，用于存储Channel对象的指针
    bool added_ = false; // 标志Channel对象是否已经添加到epoll中

    std::function<void()> readCallback_; // 可读事件的回调函数
    std::function<void()> writeCallback_; // 可写事件的回调函数
    std::function<void()> errorCallback_; // 错误事件的回调函数

public:
    Channel(int fd, EventLoop* loop);
    ~Channel();

    void handleEvent(uint32_t events);// 处理事件，根据事件类型调用相应的回调函数
    int fd() const { return fd_; } // 获取文件描述符
    uint32_t events() const { return events_; } // 获取当前关注的事件类型

    void setReadCallback(const std::function<void()>& cb) { readCallback_ = cb; }
    void setWriteCallback(const std::function<void()>& cb) { writeCallback_ = cb; }
    void setErrorCallback(const std::function<void()>& cb) { errorCallback_ = cb; }
    void enableReading(); // 使能可读事件
    void enableWriting(); // 使能可写事件
    void disableReading(); // 禁止可读事件
    void disableWriting(); // 禁止可写事件
    void disableAll();    // 移除所有事件监听，从 epoll 中删除
};

#endif