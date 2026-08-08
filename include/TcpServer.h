#pragma once
#include "Acceptor.h"
#include "TcpConnection.h"
#include "EventLoopThread.h"
#include <functional>
#include <atomic>
#include <set>
#include <mutex>

class TcpServer{
public:
    using MessageCallback = std::function<void(TcpConnection::ptr, Buffer*)>;
    using ConnectionCallback = std::function<void(TcpConnection::ptr)>;

    TcpServer(EventLoop* loop, uint16_t port, size_t numSubThreads = 0);
    ~TcpServer();

    void setMessageCallback(const MessageCallback cb);
    void setConnectionCallback(const ConnectionCallback cb);
    void start(int listenNum);
    void shutdown();
    void stopAccepting(); // 只停监听，不动现有连接（优雅排空用）
    void setMaxConnections(size_t max) { maxConnections_ = max; }
private:
    EventLoop* loop_;
    Acceptor acceptor_;
    MessageCallback messageCallback_;
    ConnectionCallback connectionCallback_;
    std::vector<std::unique_ptr<EventLoopThread>> subLoops_;
    std::set<TcpConnection::ptr> connections_;    // 持有所有活跃连接
    std::mutex connMutex_;                        // 保护 connections_ 的并发访问
    std::atomic<size_t> connectionCount_{0};
    size_t maxConnections_ = 10000;
    int next_ = 0;
};