#include "SignalHandler.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>

SignalHandler* SignalHandler::instance_ = nullptr;  // 初始化全局单例指针为 nullptr

SignalHandler::SignalHandler(EventLoop* loop)
        : eventfd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))  // 创建 eventfd + Channel
        , channel_(eventfd_, loop)
{
    // 忽略 SIGPIPE：客户端中途断开时，sendfile()/send() 会触发 SIGPIPE，
    // 默认动作是终止整个进程（服务器"自动关闭"）。忽略后 syscall 返回
    // EPIPE，由 handleClose() 正常清理该连接。
    ::signal(SIGPIPE, SIG_IGN);

    instance_ = this;  // 设置全局单例指针，供C信号处理函数使用
    channel_.setReadCallback(std::bind(&SignalHandler::handleRead, this)); // 设置可读事件的回调函数
    channel_.enableReading(); // 使能可读事件
}

SignalHandler::~SignalHandler()                 // close(eventfd)，instance_ = nullptr
{
    close(eventfd_);
    instance_ = nullptr;
}

void SignalHandler::addSignal(int signo)        // sigaction() 注册
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SignalHandler::onSignal;
    sa.sa_flags = SA_RESTART;
    sigaction(signo, &sa, nullptr);
}

void SignalHandler::setShutdownCallback(ShutdownCallback cb) { cb_ = cb; }

void SignalHandler::handleRead()                // read eventfd，调 cb_
{
    uint64_t one;
    read(eventfd_, &one, sizeof(one)); // 读 eventfd，清除可读事件
    if (cb_) cb_(); // 调用回调函数
}

void SignalHandler::onSignal(int)        // C 信号处理函数
{
    uint64_t one = 1;
    write(instance_->eventfd_, &one, sizeof(one)); // 向 eventfd 写
}