#pragma once
#include "Channel.h"
#include "Buffer.h"
#include "HttpContext.h"
#include "HttpResponse.h"
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <queue>
#include <map>


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
    // 发送文件，非阻塞，可能会分多次发送。fd 由 FileFd 共享所有权（响应多个副本共享），
    // 发送完成/连接关闭时最后一个引用释放自动 close——不在此函数内管理生命周期
    void sendFile(const std::string& headers, const FileFd& fileFd, off_t size);
    void sendResponse(const HttpResponse& resp, bool includeBody = true); // 根据isFileBody_选择发送方式；HEAD 请求传 false
    // 按请求序号投递响应（仅 IO 线程）：乱序完成先暂存，轮到序号才发送，保证响应与请求保序
    void deliverResponse(uint64_t sendSeq, const HttpResponse& resp, bool includeBody = true);
    // 请求级并行：为下一个解析完成的请求分配序号并计入在途（仅 IO 线程调用）
    uint64_t nextRequestSeq();

    void forceClose();
    void markForClose() { closeAfterSend_ = true; }  // 发送队列清空后关闭连接（HTTP/1.0 短连接）
    void connectEstablished();
    void setMessageCallback(const MessageCallback& cb);
    void setConnectionCallback(const ConnectionCallback& cb);
    void setCloseCallback(const CloseCallback& cb);
    void setOnDestroy(std::function<void()> cb) { onDestroy_ = std::move(cb); }
    HttpContext& context() { return context_; }
    // 记录最近活动时间（请求到达/连接建立时调用）；空闲超时不再每请求重建 timer，
    // 改为心跳扫描该值（timerfd 惰性重置，见 main.cpp 心跳定时器）
    void markActive();
    int64_t lastActiveTime() const { return lastActiveTime_.load(std::memory_order_relaxed); }

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
    std::atomic<int64_t> lastActiveTime_{0}; // 最近活动时间（微秒，CLOCK_MONOTONIC）
    std::atomic<bool> closed_{false};   // 防止 handleClose() 重入

    void handleRead();
    void processBufferedRequests();  // 消费 buffer 中所有完整请求（在途未达上限时）
    void handleWrite();
    void handleClose();
    void maybeCloseAfterSend();  // 短连接：数据发完且无在途请求时关闭（handleWrite 和 deliverResponse 末尾各查一次）
    void destroy();  // 仅移除 epoll 监听，不 delete this
    bool inProcess_ = false;  // 防重入：同步投递（错误/503）在解析循环内调 deliverResponse → processBufferedRequests

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
    std::function<void()> onDestroy_;

    struct SendItem {
        std::string headers;    // 响应头
        FileFd fileFd;          // 文件 fd（nullptr = 纯内存发送）；共享所有权，
                                // item 出队即释放（若为最后一个引用则关闭 fd）
        off_t fileOffset = 0;   // sendfile偏移
        off_t fileSize = 0;

        bool isFile() const { return fileFd != nullptr; }
    };

    std::queue<SendItem> sendQueue_;  // 发送队列，保证顺序发送
    bool sending_ = false;  // 是否正在发送中，防止重复调用 handleWrite()
    bool closeAfterSend_ = false;  // 队列清空后关闭连接
    uint64_t nextReqSeq_ = 0; // 下一个请求的编号
    uint64_t nextSendSeq_ = 0; // 下一个发送的响应编号
    uint64_t inflight_ = 0; // 在途请求数（已经提交但是未完成）
    // 乱序完成的响应暂存（value 含 HEAD 标志：HEAD 只发头部，暂存后发送时仍需知道）
    std::map<uint64_t, std::pair<HttpResponse, bool>> pendingResp_;
    static constexpr uint64_t kMaxInflight_ = 16; // 单连接在途请求数上限
};