#pragma once
#include "Channel.h"
#include "Buffer.h"
#include "Timer.h"
#include "HttpContext.h"
#include "HttpResponse.h"
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <queue>


class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    // 回调类型 — 全部使用 shared_ptr 防止 use-after-free
    using ptr = std::shared_ptr<TcpConnection>;
    using ConnectionCallback = std::function<void(ptr)>;
    using MessageCallback = std::function<void(ptr, Buffer*)>;
    using CloseCallback = std::function<void(ptr)>;
    
    TcpConnection(int fd, EventLoop* loop);
    ~TcpConnection();
    
    void send(const std::string& data); // 发送数据，非阻塞，可能会分多次发送
    void sendFile(const std::string& headers, const std::string& filepath, off_t size); // 发送文件，非阻塞，可能会分多次发送
    void sendResponse(const HttpResponse& resp, bool includeBody = true); // 根据isFileBody_选择发送方式；HEAD 请求传 false

    void forceClose();
    void markForClose() { closeAfterSend_ = true; }  // 发送队列清空后关闭连接（HTTP/1.0 短连接）
    void connectEstablished();
    void setMessageCallback(const MessageCallback& cb);
    void setConnectionCallback(const ConnectionCallback& cb);
    void setCloseCallback(const CloseCallback& cb);
    void setOnDestroy(std::function<void()> cb) { onDestroy_ = std::move(cb); }
    HttpContext& context() { return context_; }
    int64_t timerId() const { return timerId_; }
    void setTimerId(int64_t id) { timerId_ = id; }

    int fd() const;
    EventLoop* getLoop() const;
    void shutdown(); // 优雅关闭，停读，输出排空后自动关闭
private:
    int fd_;
    EventLoop* loop_;
    Channel channel_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;
    HttpContext context_;
    int64_t timerId_ = 0;
    std::atomic<bool> closed_{false};   // 防止 handleClose() 重入

    void handleRead();
    void handleWrite();
    void handleClose();
    void destroy();  // 仅移除 epoll 监听，不 delete this

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
    std::function<void()> onDestroy_;

    struct SendItem {
        std::string headers;    // 响应头
        std::string filePath;   // 文件路径（为空 = 纯内存发送）
        int fileFd = -1;         // 文件描述符（sendfile使用）
        off_t fileOffset = 0;   // sendfile偏移
        off_t fileSize = 0;

        bool isFile() const { return !filePath.empty(); }
    };

    std::queue<SendItem> sendQueue_;  // 发送队列，保证顺序发送
    bool sending_ = false;  // 是否正在发送中，防止重复调用 handleWrite()
    bool closeAfterSend_ = false;  // 队列清空后关闭连接
};