#include "TcpConnection.h"
#include "EventLoop.h"
#include "Log.h"
#include "Metrics.h"
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <sys/sendfile.h>

TcpConnection::TcpConnection(int fd, EventLoop* loop)
    : fd_(fd), loop_(loop), channel_(fd, loop)
{
    LOG_DEBUG << "TcpConnection created, fd=" << fd_;
}

TcpConnection::~TcpConnection()
{
    // fd 由 Channel 析构负责关闭，这里不重复 close
    LOG_DEBUG << "TcpConnection destroyed, fd=" << fd_;
}

void TcpConnection::send(const std::string& data)
{
    if (closed_) return;  // 已关闭：响应晚到（worker 慢于超时强关），客户端不可达，丢弃
    Metrics::instance().bytesSent += data.size();
    sendQueue_.push({data, "", -1, 0, 0}); // 纯内存SendItem
    if(!sending_){
        sending_ = true;
        channel_.enableWriting();
    }
}

void TcpConnection::sendFile(const std::string& headers,const std::string& filepath, off_t size)
{
    if (closed_) return;
    Metrics::instance().bytesSent += size;
    int fd = ::open(filepath.c_str(), O_RDONLY);
    if (fd < 0) {
        handleClose();
        return;
    }
    sendQueue_.push({headers, filepath, fd, 0, size});  // headers+文件
    if (!sending_) {
        sending_ = true;
        channel_.enableWriting();
    }
}

void TcpConnection::forceClose()
{
    handleClose();
}

void TcpConnection::connectEstablished()
{
    int optval = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
    setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));

    // Channel 回调使用 raw this — 安全，因为 Channel 是 TcpConnection 的成员，
    // disableAll() 后不会再触发回调，且 TcpConnection 由 shared_ptr 保证生命周期
    channel_.setReadCallback([this]() { handleRead(); });
    channel_.setWriteCallback([this]() { handleWrite(); });
    channel_.setErrorCallback([this]() { handleClose(); });
    channel_.enableReading();
    connectionCallback_(shared_from_this());
}

void TcpConnection::setMessageCallback(const MessageCallback& cb)
{
    messageCallback_ = cb;
}

void TcpConnection::setConnectionCallback(const ConnectionCallback& cb)
{
    connectionCallback_ = cb;
}

void TcpConnection::setCloseCallback(const CloseCallback& cb)
{
    closeCallback_ = cb;
}

int TcpConnection::fd() const
{
    return fd_;
}

EventLoop* TcpConnection::getLoop() const
{
    return loop_;
}

void TcpConnection::handleRead()
{
    size_t before = inputBuffer_.readableBytes();
    Buffer::ReadResult result = inputBuffer_.readFd(fd_);
    size_t received = inputBuffer_.readableBytes() - before;
    if (received > 0) Metrics::instance().bytesReceived += received;

    // ET 模式：一次 EPOLLIN 可能读到多个请求（keep-alive/pipelining），
    // 必须循环消费 buffer 中所有完整请求，否则剩余请求永远等不到新的 EPOLLIN
    processBufferedRequests();

    if (result == Buffer::kClosed) {
        // 对端 FIN（半关闭）：请求已完整送达，已接收请求的响应必须回完再关。
        // 直接 handleClose 会把在途 worker 响应丢弃（客户端挂起到空闲超时，
        // 实测：发请求 + SHUT_WR 后收不到响应）。shutdown() = 停读 + 排空后自动关闭
        shutdown();
    } else if (result == Buffer::kError) {
        handleClose();
    }
}

void TcpConnection::processBufferedRequests()
{
    // processing_ 串行化：同一连接同时至多一个请求在 worker 池中。
    // 多线程池下若同时处理多个流水线请求，完成顺序不确定 → queueInLoop
    // 入队顺序竞态 → 响应错配（RFC 7230 §6.3.2 要求响应与请求一一对应保序）。
    // 剩余请求在 endRequest() 里继续处理，顺序天然保持
    while (!closed_ && !processing_ && inputBuffer_.readableBytes() > 0) {
        size_t oldLen = inputBuffer_.readableBytes();
        messageCallback_(shared_from_this(), &inputBuffer_);
        // parseRequest 没消费任何数据 = 请求不完整，等更多数据（下次 EPOLLIN）
        if (inputBuffer_.readableBytes() >= oldLen) break;
    }
}

void TcpConnection::beginRequest()
{
    processing_ = true;  // 请求提交到 worker：本连接暂停解析新请求
}

void TcpConnection::endRequest()
{
    // 响应已入发送队列：解除串行锁，继续处理 buffer 中剩余的流水线请求。
    // 连接已关闭（超时强关等）时 closed_ 短路，不再解析
    processing_ = false;
    processBufferedRequests();
}

void TcpConnection::handleWrite()
{
    while (!sendQueue_.empty()) {
        SendItem& item = sendQueue_.front();

        // 先发内存部分（headers）
        if (!item.headers.empty()) {
            ssize_t n = ::send(fd_, item.headers.data(),
                               item.headers.size(), MSG_NOSIGNAL);
            if (n > 0) {
                item.headers.erase(0, n);
                if (!item.headers.empty()) return;  // 没发完，等下次 EPOLLOUT
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            } else {
                handleClose();
                return;
            }
        }
        // headers 发完，再发文件部分
        else if (item.isFile()) {
            // 空文件（0 字节）：跳过 sendfile，直接结束本项。
            // sendfile 对 count=0 返回 0，会误落入 errno 错误分支导致误关连接
            if (item.fileOffset >= item.fileSize) {
                ::close(item.fileFd);
                sendQueue_.pop();
                continue;
            }
            ssize_t n = ::sendfile(fd_, item.fileFd, &item.fileOffset,
                                   item.fileSize - item.fileOffset);
            if (n > 0) {
                if (item.fileOffset < item.fileSize) return;  // 没发完
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            } else {
                handleClose();
                return;
            }
            // 文件发完，关闭 fd
            ::close(item.fileFd);
            sendQueue_.pop();
        }
        // headers + 文件发完
        else {
            sendQueue_.pop();  // 纯内存且 headers 已空
        }
    }
    // 队列空 → 关闭写事件
    sending_ = false;
    channel_.disableWriting();
    // 短连接（HTTP/1.0）：所有数据发送完毕后关闭连接。
    // !processing_：还有请求在 worker 池中时不能关——它的响应尚未入队，
    // 此刻关闭会把前序响应一起丢掉（实测：流水线 + 报错 → 响应错配）
    if (closeAfterSend_ && !processing_) {
        handleClose();
    }
}

void TcpConnection::handleClose()
{
    // 防止重入：messageCallback_ 中调了 forceClose()，
    // handleRead/handleWrite 后续又会再次调用 handleClose()
    if (closed_.exchange(true)) return;

    // 守卫：保持 shared_ptr 直到函数结束，防止 closeCallback_ 中释放
    // 最后一个引用导致 this 被 delete，后续 destroy() 访问 UAF
    ptr guard = shared_from_this();

    LOG_DEBUG << "Connection closing, fd=" << fd_;

    // 清理发送队列中未发送完的文件 fd，防止 fd 泄漏
    // （客户端中途断开时，SendItem 里的 open fd 不会走正常 handleWrite 关闭）
    while (!sendQueue_.empty()) {
        if (sendQueue_.front().fileFd >= 0) ::close(sendQueue_.front().fileFd);
        sendQueue_.pop();
    }

    if (onDestroy_) onDestroy_();
    if (closeCallback_) closeCallback_(guard);
    destroy();
}

void TcpConnection::shutdown() // 优雅关闭，停读，输出排空后自动关闭
{
    channel_.disableReading(); //不在触发handleRead，拒绝新请求
    markForClose(); // 标记为关闭
    // 没有待发送数据且没有在途请求（worker 池中）才立即关：
    // processing_ 为 true 时关闭会把 worker 尚未入队的响应一起丢掉
    //（实测：半关闭 FIN 到达时请求刚提交 worker → 客户端收不到响应）
    if(sendQueue_.empty() && !sending_ && !processing_){
        handleClose(); // 没有待发送数据，直接关闭
    }
}

void TcpConnection::destroy()
{
    // 从 epoll 移除，不再监听任何事件
    channel_.disableAll();

    // 延迟释放守卫：epoll_wait 返回的 events 数组可能还持有本 Channel 的指针，
    // 如果此时析构 TcpConnection，后续遍历 events 时 Channel 指针会悬空。
    // 将 shared_ptr 放入 pending queue，确保析构发生在 epoll for 循环之后。
    loop_->queueInLoop([guard = shared_from_this()]() {
        // guard 在此销毁，TcpConnection 生命周期安全结束
    });
}

void TcpConnection::sendResponse(const HttpResponse& resp, bool includeBody) // 根据isFileBody_选择发送方式
{
    if (resp.isFileBody()) {
        if (includeBody) {
            sendFile(resp.headersToString(), resp.fileBodyPath(), resp.fileBodySize());
        } else {
            send(resp.headersToString());  // HEAD：只发头部（头里已有真实 Content-Length），不发文件本体
        }
    } else {
        send(resp.toString(includeBody));
    }
}
