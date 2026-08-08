#pragma once
#include <sys/socket.h>
#include <functional>
#include <netinet/in.h>
#include "EventLoop.h"
#include "Channel.h"


class Acceptor{
public:
    using NewConnectionCallback = std::function<void(int sockfd,const sockaddr_in& peerAddr)>;

    Acceptor(EventLoop* loop, uint16_t port);
    ~Acceptor();

    void setNewConnectionCallback(const NewConnectionCallback cb);//设置新连接回调函数
    int fd();//返回fd
    void listen(int listenNum);//设置监听队列大小,开启监听
    void close();  // 关闭监听socket

private:
    void handleRead();  //处理监听

    int listenfd_;  //监听fd
    EventLoop* loop_;//事件循环
    Channel channel_;//监听channel
    NewConnectionCallback newConnectionCallback_;//新连接回调
};