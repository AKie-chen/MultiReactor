#include "TcpServer.h"
#include "Log.h"
#include "Metrics.h"
#include <fcntl.h>
#include <unistd.h>

TcpServer::TcpServer(EventLoop* loop, uint16_t port, size_t numSubThreads)
    : loop_(loop),
      acceptor_(loop_, port)
{
    for (size_t i = 0; i < numSubThreads; ++i) {
        subLoops_.push_back(std::unique_ptr<EventLoopThread>(new EventLoopThread()));
    }
}

TcpServer::~TcpServer() {}

void TcpServer::setMessageCallback(const MessageCallback cb)
{
    messageCallback_ = cb;
}

void TcpServer::setConnectionCallback(const ConnectionCallback cb)
{
    connectionCallback_ = cb;
}

void TcpServer::start(int listenNum)
{
    acceptor_.setNewConnectionCallback([this](int client_fd, const sockaddr_in& client_addr) {

        if (connectionCount_ >= maxConnections_) {
            ::close(client_fd);
            return;
        }

        connectionCount_++;
        Metrics::instance().activeConnections++;
        EventLoop* ioLoop = subLoops_.empty() ? loop_ : subLoops_[next_++ % subLoops_.size()]->getLoop();

        auto conn = std::make_shared<TcpConnection>(client_fd, ioLoop);
        {
            std::lock_guard<std::mutex> lock(connMutex_);
            connections_.insert(conn);
        }

        // 连接关闭时从集合中移除，释放 server 持有的 shared_ptr
        conn->setOnDestroy([this, raw = conn.get()]() {
            connectionCount_--;
            Metrics::instance().activeConnections--;
            {
                std::lock_guard<std::mutex> lock(connMutex_);
                for (auto it = connections_.begin(); it != connections_.end(); ++it) {
                    if (it->get() == raw) {
                        connections_.erase(it);
                        break;
                    }
                }
            }
        });

        conn->setMessageCallback(messageCallback_);
        conn->setConnectionCallback([this](TcpConnection::ptr c) {
            this->connectionCallback_(c);
        });

        fcntl(client_fd, F_SETFL, O_NONBLOCK);
        ioLoop->queueInLoop([conn]() {
            conn->connectEstablished();
        });
        LOG_DEBUG << "Accepted new connection, fd=" << client_fd;
    });
    acceptor_.listen(listenNum);
}

void TcpServer::stopAccepting() // 只停监听，不动现有连接（优雅排空用）
{
    acceptor_.close();
}

void TcpServer::forEachConnection(const std::function<void(const TcpConnection::ptr&)>& fn)
{
    std::lock_guard<std::mutex> lock(connMutex_);
    for (const auto& conn : connections_) {
        fn(conn);
    }
}

void TcpServer::shutdown()
{
    acceptor_.close();      // 1. 停止接受新连接
    std::vector<TcpConnection::ptr> conns;
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        conns.assign(connections_.begin(), connections_.end()); // 2. 复制当前连接
    }

    // 3. 每个连接在自己所属的 IO 线程里优雅关闭。
    //    之前主线程直接调 conn->shutdown()：TcpConnection 的 Channel/发送队列/定时器
    //    都是所属 IO 线程独占的，跨线程触碰会与 IO 线程的操作并发（closeCallback →
    //    TimerQueue::cancel 和 IO 线程的 handleRead 同时改 unordered_map 等），
    //    导致堆损坏 → SIGSEGV（实测：connections_ 红黑树遍历崩溃 + destroy() 抛
    //    bad_weak_ptr）。用 queueInLoop 把关闭动作投递到所属线程执行
    for (auto& conn : conns) {
        conn->getLoop()->queueInLoop([conn]() { conn->shutdown(); });
    }

    // 4. 停 IO 线程：quit() 唤醒 epoll_wait；loop() 每轮迭代先执行 pendingFunctors_
    //    再检查退出标志，所以上面入队的 shutdown 一定先于线程退出执行
    for (auto& loopThread : subLoops_) {
        loopThread->getLoop()->quit();
    }
    subLoops_.clear();      // 5. EventLoopThread 析构:loop 已 quit,join 立即返回
}