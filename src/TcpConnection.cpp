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
#include <time.h>

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

    if (!sending_) {
        // 队列空闲：先直接非阻塞写（消除每请求一次 enableWriting 的 epoll_ctl MOD）。
        // 小响应（socket 缓冲区装得下）一次写完 → 0 次 epoll_ctl；EAGAIN/部分写
        // 才入队并注册 EPOLLOUT。EPOLLOUT 注册后永不注销——ET 只在"不可写→可写"
        // 状态变化时触发，注册着不触发 = 零成本，下一次腾空自动再触发
        ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(data.size())) return;  // 一次写完
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            handleClose();  // EPIPE/ECONNRESET：对端已关闭
            return;
        }
        if (n > 0) sendQueue_.push({data.substr(n), {}, 0, 0});  // 部分写：剩余入队
        else sendQueue_.push({data, {}, 0, 0});  // n==0 或 EAGAIN：整体入队
        sending_ = true;
        if (!channel_.isWritingEnabled()) channel_.enableWriting();  // 已注册则跳过冗余 MOD
        return;
    }
    // 发送中（队列非空/等 EPOLLOUT）：按序入队，handleWrite 会继续发
    sendQueue_.push({data, {}, 0, 0}); // 纯内存SendItem
}

void TcpConnection::sendFile(const std::string& headers, const FileFd& fileFd, off_t size)
{
    // fd 由响应持有（FileFd 共享所有权），本函数不 open/close：关闭时机由
    // 最后一个引用（响应副本 / SendItem）释放决定——发送中、暂存中、连接死亡
    // 三条路径都恰好关闭一次，无泄漏也无提前关闭
    if (closed_) return;   // 已关闭：响应晚到，客户端不可达（fd 随响应副本析构关闭）
    if (!fileFd) {
        handleClose();
        return;
    }
    Metrics::instance().bytesSent += size;

    if (!sending_) {
        // 同 send()：队列空闲先直接写 headers，成功后再试 sendfile 文件体。
        // 小文件（socket 缓冲区装得下）一次发完 → 0 次 epoll_ctl
        ssize_t n = ::send(fd_, headers.data(), headers.size(), MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(headers.size())) {
            off_t offset = 0;
            ssize_t m = ::sendfile(fd_, fileFd->fd, &offset, size);
            if (m >= 0 && offset == size) {  // headers + 文件一次发完（空文件 size==0 也在此命中）
                return;
            }
            if (m < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                handleClose();
                return;
            }
            // 文件部分发完/EAGAIN：剩余入队等 EPOLLOUT（headers 已发完，SendItem 不带 headers）
            sendQueue_.push({"", fileFd, offset, size});
            sending_ = true;
            if (!channel_.isWritingEnabled()) channel_.enableWriting();
            return;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            handleClose();
            return;
        }
        // headers 部分发完/EAGAIN：headers 剩余 + 整个文件入队
        if (n > 0) sendQueue_.push({headers.substr(n), fileFd, 0, size});
        else sendQueue_.push({headers, fileFd, 0, size});
        sending_ = true;
        if (!channel_.isWritingEnabled()) channel_.enableWriting();
        return;
    }
    // 发送中：按序入队，handleWrite 会从 headers 开始发
    sendQueue_.push({headers, fileFd, 0, size});  // headers+文件
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
    // 防重入：同步投递路径（解析错误/503）在 messageCallback 内调 deliverResponse，
    // deliverResponse 末尾会再调本函数——直接返回，外层循环继续处理即可
    if (inProcess_) return;
    inProcess_ = true;

    // 请求级并行：只要在途未达上限就继续解析+提交流水线请求（乱序处理）。
    // 多线程池完成顺序不确定 → 响应必须靠 deliverResponse 按 seq 重排发送
    // （RFC 7230 §6.3.2 要求响应与请求一一对应保序），否则响应错配
    while (!closed_ && inflight_ < kMaxInflight_ && inputBuffer_.readableBytes() > 0) {
        size_t oldLen = inputBuffer_.readableBytes();
        messageCallback_(shared_from_this(), &inputBuffer_);
        // parseRequest 没消费任何数据 = 请求不完整，等更多数据（下次 EPOLLIN）
        if (inputBuffer_.readableBytes() >= oldLen) break;
    }

    inProcess_ = false;
}

// 为下一个解析完成的请求分配序号并计入在途（仅 IO 线程）。
// 成功提交、解析错误、503 三条路径都各调用一次；错误/503 随后同步
// deliverResponse 会立即抵消 inflight_ 计数
uint64_t TcpConnection::nextRequestSeq()
{
    ++inflight_;
    return nextReqSeq_++;
}

// 按请求序号投递响应（仅 IO 线程）：乱序完成先暂存，轮到序号才发送。
// 发送队列的入队顺序 = seq 递增顺序，因此响应与请求保序（RFC 7230 §6.3.2）
void TcpConnection::deliverResponse(uint64_t sendSeq, const HttpResponse& resp, bool includeBody)
{
    if (closed_) return;  // 连接已关：worker 晚到响应丢弃
    if (inflight_ > 0) --inflight_;  // 任务完成（无论暂存还是直发）

    if (sendSeq == nextSendSeq_) {
        // 前序全部发完，本响应直接发送
        sendResponse(resp, includeBody);
        ++nextSendSeq_;
        // 顺藤摸瓜排空连续段：map 中按序补发之前暂存的响应
        while (true) {
            auto it = pendingResp_.find(nextSendSeq_);
            if (it == pendingResp_.end()) break;
            sendResponse(it->second.first, it->second.second);
            pendingResp_.erase(it);
            ++nextSendSeq_;
        }
    } else if (sendSeq > nextSendSeq_) {
        pendingResp_[sendSeq] = {resp, includeBody};  // 前序未完成，暂存（拷贝）
    } else {
        // sendSeq < nextSendSeq_：重复/非法投递（不应发生），丢弃
    }

    processBufferedRequests();  // 释放在途名额，继续解析后续流水线请求
    maybeCloseAfterSend();      // 直接写路径没有 EPOLLOUT 事件触发 handleWrite，需在此评估短连接关闭
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
                sendQueue_.pop();  // 释放引用：最后一个引用时自动关闭 fd
                continue;
            }
            ssize_t n = ::sendfile(fd_, item.fileFd->fd, &item.fileOffset,
                                   item.fileSize - item.fileOffset);
            if (n > 0) {
                if (item.fileOffset < item.fileSize) return;  // 没发完
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            } else {
                handleClose();
                return;
            }
            // 文件发完，出队释放 fd（最后一个引用时自动关闭）
            sendQueue_.pop();
        }
        // headers + 文件发完
        else {
            sendQueue_.pop();  // 纯内存且 headers 已空
        }
    }
    // 队列空 → 写事件保持注册（ET 下不触发 = 零成本），sending_ 复位等待下次直接写
    sending_ = false;
    maybeCloseAfterSend();
}

void TcpConnection::markActive()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    lastActiveTime_.store(ts.tv_sec * 1'000'000 + ts.tv_nsec / 1'000, std::memory_order_relaxed);
}

void TcpConnection::maybeCloseAfterSend()
{
    // 短连接（HTTP/1.0 / Connection: close / 排空期）：所有数据发送完毕后关闭连接。
    // inflight_ == 0 && pendingResp_.empty() : 还有请求在 worker 池中时不能关——它的响应尚未入队，
    // 此刻关闭会把前序响应一起丢掉（实测：流水线 + 报错 → 响应错配）。
    // 原逻辑只在 handleWrite 检查；直接写路径没有 EPOLLOUT 事件触发 handleWrite，
    // 必须在 deliverResponse 末尾也检查一次，否则短连接永不关闭（fd 泄漏）
    if (closeAfterSend_ && sendQueue_.empty() && !sending_ && inflight_ == 0 && pendingResp_.empty()) {
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

    // 清理发送队列中未发送完的文件 fd：pop 释放 FileFd 引用，最后一个引用时自动
    // 关闭（客户端中途断开时，SendItem 里的 fd 不会走正常 handleWrite 的释放路径；
    // 响应暂存表 pendingResp_ 里的 fd 同理由各自 shared_ptr 在析构时关闭）
    while (!sendQueue_.empty()) {
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
    //（实测：半关闭 FIN 到达时请求刚提交 worker → 客户端收不到响应）
    if(sendQueue_.empty() && !sending_ && inflight_ == 0 && pendingResp_.empty()){
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
            sendFile(resp.headersToString(), resp.fileFd(), resp.fileBodySize());
        } else {
            send(resp.headersToString());  // HEAD：只发头部（头里已有真实 Content-Length），不发文件本体
        }
    } else {
        send(resp.toString(includeBody));
    }
}
