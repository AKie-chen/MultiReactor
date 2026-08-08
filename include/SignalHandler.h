#pragma once
#include "Channel.h"

class EventLoop;

class SignalHandler {
public:
    using ShutdownCallback = std::function<void()>;    
    
    SignalHandler(EventLoop* loop);   // 创建 eventfd + Channel，enableReading
    ~SignalHandler();                 // close(eventfd)，instance_ = nullptr
    
    void addSignal(int signo);        // sigaction() 注册
    void setShutdownCallback(ShutdownCallback cb);  // 设置回调
    void handleRead();                // read eventfd，调 cb_
    
private:
    int eventfd_;               // eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)
    Channel channel_;           // 包装 eventfd，注册到 EventLoop
    ShutdownCallback cb_;       // 信号来了调什么
    
    static SignalHandler* instance_;  // 全局单例指针，给 C 函数用
    static void onSignal(int);        // C 信号处理函数
};