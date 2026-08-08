#include "Acceptor.h"
#include "Log.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

Acceptor::Acceptor(EventLoop* loop, uint16_t port)
            :loop_(loop),
            listenfd_(socket(AF_INET,SOCK_STREAM,0)), 
            channel_(listenfd_,loop_)
{
    // socket 失败:fd 为 -1,后续 bind 必然失败,直接中止并说明原因
    if (listenfd_ < 0) {
        LOG_FATAL << "socket() failed: " << strerror(errno);
    }

    fcntl(listenfd_, F_SETFL, O_NONBLOCK); //将socket设置为非阻塞模式

    sockaddr_in addr; //定义一个sockaddr_in结构体，用于存储服务器的地址信息
    addr.sin_family=AF_INET; //设置地址族为IPv4
    addr.sin_port=htons(port); //设置端口号为8080
    addr.sin_addr.s_addr=INADDR_ANY; //设置IP地址为任意地址

    int optval = 1;
    setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)); //设置socket选项，允许地址重用

    // bind 失败(如端口已被占用)必须立即暴露,绝不能带病继续:
    // 否则进程带着未绑定的 fd 静默进入事件循环,永远 accept 不到连接,
    // "正常启动"却一个请求都不处理(实测:残留进程占端口时的现象)
    if (bind(listenfd_,(sockaddr*)&addr,sizeof(addr)) < 0) {
        LOG_FATAL << "bind() port " << port << " failed: " << strerror(errno);
    }
}

Acceptor::~Acceptor(){ close(); }

void Acceptor::close() {
    if (listenfd_ >= 0) {
        channel_.disableAll();   // 同时停止监听：防止 close 后 epoll 残留事件再触发 accept 回调
        ::close(listenfd_);
        listenfd_ = -1;
    }
}

void Acceptor::setNewConnectionCallback(const NewConnectionCallback cb)//设置新连接回调函数
{
    newConnectionCallback_ = cb;
}

int Acceptor::fd()//返回fd
{
    return listenfd_;
}

void Acceptor::listen(int listenNum)//开启监听
{
    // listenNum 为内核排队连接数上限(backlog),由调用方传入
    if (::listen(listenfd_,listenNum) < 0) {
        LOG_FATAL << "listen() failed: " << strerror(errno);
    }
    handleRead();
}

void Acceptor::handleRead()  //处理监听
{
    channel_.setReadCallback([&](){ // 设置可读事件的回调函数
        if (listenfd_ < 0) return;  // close() 后残留批次事件，不再 accept
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd;

        while((client_fd = accept(channel_.fd(), (sockaddr*)&client_addr, &client_len)) != -1){//循环接受连接，直到没有连接请求为止
            newConnectionCallback_(client_fd,client_addr);//
        }

        if(errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR << "accept error" << strerror(errno);
        }
    });
    channel_.enableReading(); // 使能可读事件
}