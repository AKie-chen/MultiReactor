#include "Log.h"
#include "EventLoop.h"
#include "Channel.h"
#include "Buffer.h"
#include "TcpConnection.h"
#include "TcpServer.h"
#include "HttpContext.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "TimerQueue.h"
#include "ThreadPool.h"
#include <signal.h>
#include "SignalHandler.h"
#include "Router.h"
#include "StaticFileHandler.h"
#include "Config.h"
#include "Metrics.h"
#include <string>
#include <sys/resource.h>
#include <cstring>

int main(int argc, char* argv[]) {
    ServerConfig cfg;
    ConfigParser parser;
    if (!parser.parse(argc, argv, cfg)) {
        return 0;
    }

    Logger::setLevel(LogLevel(Logger::parseLogLevel(cfg.logLevel)));

    // fd 上限处理（fd 耗尽是真实故障源：accept 返回 EMFILE + epoll 忙循环，
    // 见 Acceptor::handleRead 的 EMFILE 排空逻辑）：
    // 1. 软上限提到硬上限，让进程可用 fd 最大化
    // 2. maxConnections 与 fd 上限联动：连接 fd 之外还有 listen/epoll/eventfd/
    //    timerfd/日志/sendFile 等固定消耗，预留余量，防止 maxConnections 远大于
    //    ulimit 时连接数没到上限就撞上 EMFILE
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            LOG_WARN << "setrlimit(RLIMIT_NOFILE) to " << rl.rlim_max << " failed: " << strerror(errno);
        }
        const size_t kFdReserve = 64; // 给 listen/epoll/eventfd/timerfd/日志/sendFile 预留
        size_t fdCap = rl.rlim_cur > kFdReserve ? static_cast<size_t>(rl.rlim_cur - kFdReserve) : 0;
        if (cfg.maxConnections > fdCap) {
            LOG_WARN << "maxConnections " << cfg.maxConnections << " exceeds fd limit "
                     << rl.rlim_cur << ", clamped to " << fdCap;
            cfg.maxConnections = fdCap;
        }
    }

    EventLoop loop;
    TcpServer server(&loop, cfg.port, cfg.ioThreads);
    ThreadPool threadPool(cfg.workerThreads, cfg.maxQueueSize);

    // 排空检查函数：main 作用域，而不是 shutdown 回调的局部变量。
    // 定时器链只按引用捕获它——main 的栈帧在整个排空期间（最长 10s）存活，
    // 引用始终有效；若放回回调内部则回调一返回就悬垂（UAF 实测），
    // 而用 shared_ptr 自持会形成引用环（lambda 捕获自身所属对象的
    // shared_ptr）导致泄漏（LSAN 实测 80B）
    std::function<void()> drainCheck;

    std::atomic<bool> isShutdown(false);
    SignalHandler signalHandler(&loop);
    signalHandler.addSignal(SIGINT);
    signalHandler.addSignal(SIGTERM);
    signalHandler.setShutdownCallback([&loop, &server, &drainCheck, &isShutdown, &threadPool]() {
        LOG_INFO << "Graceful shutdown: stop accepting, draining connections...";

        // 1. 只停监听：关闭 acceptor，不再接受新连接（现有连接继续服务完毕）
        server.stopAccepting();
        isShutdown.store(true);

        // 2. 每 100ms 检查一次活跃连接数，归零或超 10s 则退出主循环
        timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t deadline = (ts.tv_sec * 1'000'000 + ts.tv_nsec / 1'000) + 10 * 1'000'000;

        drainCheck = [&loop, deadline, &drainCheck, &threadPool]() {
            int64_t active = Metrics::instance().activeConnections.load();
            timespec t2; clock_gettime(CLOCK_MONOTONIC, &t2);
            int64_t now = t2.tv_sec * 1'000'000 + t2.tv_nsec / 1'000;
            LOG_DEBUG << "drain check: active=" << active << " now=" << now << " deadline=" << deadline;
            if ((active == 0 && threadPool.inFlight() == 0) || now >= deadline) {
                // 兜底必须短路：deadline 到了无论还有没有在途任务都要退出，
                // 否则 & 绑定下 deadline 变成"再等 100ms"，10s 保证失效（可能永远挂住）
                loop.quit();   // 现在才退出主循环。注意不能提前 quit()：
                               // loop() 的 while 条件在顶部，提前 quit 本迭代结束就退出，
                               // 之后 addTimer 的检查永远不会再触发（原实现的坑）
                return;
            }
            loop.timerQueue().addTimer([&loop, &drainCheck]() { drainCheck(); }, now + 100'000);  // 100ms
        };
        drainCheck();
    });

    Router router;
    StaticFileHandler staticHandler(cfg.staticDir, cfg.maxFileSizeMB * 1024 * 1024);

    router.addRoute(HttpRequest::kGet, "/", [](const HttpRequest& req, HttpResponse* resp, const std::map<std::string, std::string>& params) {
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setBody("Hello, World!");
        resp->addHeader("Content-Length", std::to_string(resp->body().size()));
        resp->addHeader("Content-Type", "text/plain");
    });

    router.addRoute(HttpRequest::kGet, "/stats", [](const HttpRequest&, HttpResponse* resp, const std::map<std::string, std::string>& params) {
        std::string json = Metrics::instance().toJson();
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setBody(json);
        resp->addHeader("Content-Type", "application/json");
        resp->addHeader("Content-Length", std::to_string(json.size()));
    });

    router.addRoute(HttpRequest::kGet, "/user/:id", [](const HttpRequest&, HttpResponse* resp, const std::map<std::string, std::string>& params) {
        std::string body = "user id: " + params.at("id");
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setBody(body);
        resp->addHeader("Content-Length", std::to_string(body.size()));
        resp->addHeader("Content-Type", "text/plain");
    });



    // timerfd 惰性重置：空闲超时不再每请求 cancel+addTimer（每请求一次
    // timerfd_settime——单连接压测下新 timer 总是堆顶，earliestChanged 恒为真）。
    // 改为：连接只记 lastActiveTime_（零 syscall），下面注册的 1s 心跳定时器扫描
    // 所有连接，超时才强关。timerfd_settime 从每请求 1 次 → 启动时 1 次；
    // 代价是超时精度从精确毫秒降为 ±1s（keep-alive 场景可接受）
    server.setConnectionCallback([](TcpConnection::ptr conn) {
        conn->markActive();  // 空闲超时从连接建立起算
    });

    server.setMessageCallback([&threadPool, &router, &staticHandler, &isShutdown]
                              (TcpConnection::ptr conn, Buffer* buf) {
        // 排空期（SIGTERM 后）：请求照常处理，但响应后连接即关闭。
        // 不能静默丢弃——客户端已发出的请求必须得到响应，否则挂起直到超时
        // （实测：keep-alive 连接在 shutdown 后发请求，无响应挂 3s）。
        // markForClose 在发送队列清空后自动关连接，排空自然收敛
        if (isShutdown) conn->markForClose();

        HttpContext& ctx = conn->context();

        // 解析请求（解析状态累积在 ctx 内部，TCP 拆包时多次 EPOLLIN 之间不丢失）
        if (!ctx.parseRequest(buf)) {
            if (ctx.error() == HttpContext::kNoError) {
                // RFC 7231 §5.1.1：请求带 Expect: 100-continue 且头部已解析完（等待 body）→
                // 立即回 100 Continue，客户端才会发送 body。每个请求只回一次（continueSent_）。
                // 注意顺序：413 等拒绝已在 parseRequest 里先于状态转移检查，不会发完 100 再反悔
                if (ctx.shouldSendContinue()) {
                    conn->send("HTTP/1.1 100 Continue\r\n\r\n");
                    ctx.markContinueSent();
                }
                return; // 请求不完整，继续等待
            }
            else if (ctx.error() == HttpContext::kBadRequest) { // 400 Bad Request
                Metrics::instance().errors4xx++;
                // 错误响应也按当前请求的 seq 定序：前面在途响应未发完时先暂存
                HttpResponse errResp = HttpResponse::makeError(
                    HttpResponse::k400BadRequest, "Bad Request");
                // 先标记再投递：direct 写成功路径没有 EPOLLOUT 事件触发 handleWrite，
                // deliverResponse 末尾的 maybeCloseAfterSend 是唯一评估点（实测：顺序
                // 放反会一直挂到心跳空闲超时才关连接）
                conn->markForClose();
                conn->deliverResponse(conn->nextRequestSeq(), errResp, true);
                ctx.reset();
                return;
            } else if (ctx.error() == HttpContext::kMethodNotSupported) { // 405 Method Not Allowed
                Metrics::instance().errors4xx++;
                HttpResponse errResp = HttpResponse::makeError(
                    HttpResponse::k405MethodNotAllowed,
                    "Method Not Supported");
                // 先标记再投递：direct 写成功路径没有 EPOLLOUT 事件触发 handleWrite，
                // deliverResponse 末尾的 maybeCloseAfterSend 是唯一评估点（实测：顺序
                // 放反会一直挂到心跳空闲超时才关连接）
                conn->markForClose();
                conn->deliverResponse(conn->nextRequestSeq(), errResp, true);
                ctx.reset();
                return;
            } else if (ctx.error() == HttpContext::kVersionNotSupported) { // 505 HTTP Version Not Supported
                Metrics::instance().errors5xx++;
                HttpResponse errResp = HttpResponse::makeError(
                    HttpResponse::k505HttpVersionNotSupported,
                    "Http Version Not Supported");
                // 先标记再投递：direct 写成功路径没有 EPOLLOUT 事件触发 handleWrite，
                // deliverResponse 末尾的 maybeCloseAfterSend 是唯一评估点（实测：顺序
                // 放反会一直挂到心跳空闲超时才关连接）
                conn->markForClose();
                conn->deliverResponse(conn->nextRequestSeq(), errResp, true);
                ctx.reset();
                return;
            } else if (ctx.error() == HttpContext::kHeaderTooLarge) { // 413 Header Too Large
                Metrics::instance().errors4xx++;
                HttpResponse errResp = HttpResponse::makeError(
                    HttpResponse::k413PayloadTooLarge,
                    "Request Header Too Large");
                // 先标记再投递：direct 写成功路径没有 EPOLLOUT 事件触发 handleWrite，
                // deliverResponse 末尾的 maybeCloseAfterSend 是唯一评估点（实测：顺序
                // 放反会一直挂到心跳空闲超时才关连接）
                conn->markForClose();
                conn->deliverResponse(conn->nextRequestSeq(), errResp, true);
                ctx.reset();
                return;
            } else if (ctx.error() == HttpContext::kNotImplemented) { // 501 Not Implemented (Transfer-Encoding)
                Metrics::instance().errors5xx++;
                HttpResponse errResp = HttpResponse::makeError(
                    HttpResponse::k501NotImplemented,
                    "Not Implemented");
                // 先标记再投递：direct 写成功路径没有 EPOLLOUT 事件触发 handleWrite，
                // deliverResponse 末尾的 maybeCloseAfterSend 是唯一评估点（实测：顺序
                // 放反会一直挂到心跳空闲超时才关连接）
                conn->markForClose();
                conn->deliverResponse(conn->nextRequestSeq(), errResp, true);
                ctx.reset();
                return;
            } else if (ctx.error() == HttpContext::kExpectationFailed) { // 417 Expectation Failed
                Metrics::instance().errors4xx++;
                HttpResponse errResp = HttpResponse::makeError(
                    HttpResponse::k417ExpectationFailed,
                    "Expectation Failed");
                // 先标记再投递：direct 写成功路径没有 EPOLLOUT 事件触发 handleWrite，
                // deliverResponse 末尾的 maybeCloseAfterSend 是唯一评估点（实测：顺序
                // 放反会一直挂到心跳空闲超时才关连接）
                conn->markForClose();
                conn->deliverResponse(conn->nextRequestSeq(), errResp, true);
                ctx.reset();
                return;
            }
        }

        // 取出解析结果，处理完复位，等待下一个请求
        HttpRequest req = ctx.request();
        ctx.reset();
        Metrics::instance().totalRequests++;
        conn->markActive();  // 刷新空闲超时计时（心跳扫描用，零 syscall）

        // 请求级并行：分配序号后独立提交 worker（同一连接多个请求同时在池中）。
        // 响应乱序完成、由 deliverResponse 按序发送，替代原 processing_ 串行化
        uint64_t seq = conn->nextRequestSeq();
        bool submitted = threadPool.tryRun([conn, req, seq, &router, &staticHandler, &isShutdown]() {
            HttpResponse resp;

            // 异常隔离（业务层）：路由 handler / 静态文件处理抛异常（如用户
            // handler 的 params.at() 缺键、std::bad_alloc 等）→ 统一转 500。
            // 客户端得到显式错误而非挂起等待超时；再往外是 ThreadPool 包裹层的
            // 兜底（保证 worker 线程存活 + 在途计数平衡）
            try {
                // 处理路由
                std::map<std::string, std::string> params;
                auto result = router.route(req, &resp, &params);
                if (result == RouterResult::kNotFound) {
                    if (!staticHandler.handle(req, &resp)) {
                    resp = HttpResponse::makeError(HttpResponse::k404NotFound, "Not Found");
                    }
                } else if (result == RouterResult::kMethodNotAllowed) {
                    resp = HttpResponse::makeError(HttpResponse::k405MethodNotAllowed, "Method Not Allowed");
                }
            } catch (const std::exception& e) {
                LOG_ERROR << "Handler exception for " << req.path() << ", responding 500: " << e.what();
                resp = HttpResponse::makeError(HttpResponse::k500InternalServerError, "Internal Server Error");
                Metrics::instance().errors5xx++;
            } catch (...) {
                LOG_ERROR << "Unknown handler exception for " << req.path() << ", responding 500";
                resp = HttpResponse::makeError(HttpResponse::k500InternalServerError, "Internal Server Error");
                Metrics::instance().errors5xx++;
            }

            int code = static_cast<int>(resp.code());
            if (code >= 400 && code < 500) Metrics::instance().errors4xx++;
            else if (code >= 500) Metrics::instance().errors5xx++;

            // HTTP/1.1 默认长连接；客户端显式 Connection: close → 响应后关闭（RFC 7230 §6.3）
            bool closeConn = resp.closeConnection();
            if (req.getHeader("Connection") == "close") {
                closeConn = true;
                resp.setCloseConnection(true);
            }
            // HTTP/1.0 默认短连接：响应发送完毕后关闭连接
            if (req.version() == "HTTP/1.0" && !closeConn) {
                closeConn = true;
                resp.setCloseConnection(true);
            }
            // 排空期：即使 keep-alive 也强制 Connection: close，
            // 每个连接处理完当前请求即关闭，排空才能收敛（否则连接永远挂着）
            if (isShutdown && !closeConn) {
                closeConn = true;
                resp.setCloseConnection(true);
            }

            // HEAD 只发头部（RFC 7231 §4.3.2）：Content-Length 保留真实大小，body/文件本体不发
            bool isHead = (req.method() == HttpRequest::kHead);
            conn->getLoop()->queueInLoop([conn, seq, resp, closeConn, isHead]() {
                // 先标记再投递：markForClose 若在 deliverResponse 之后，末尾的
                // maybeCloseAfterSend 看不到标志；direct 写成功又无 EPOLLOUT 事件
                // 补评，短连接会一直挂到心跳空闲超时才关闭（fd 长时间占用）
                if (closeConn) conn->markForClose();
                // 按序投递：乱序完成的响应由 deliverResponse 暂存/重排，保证响应与请求保序
                conn->deliverResponse(seq, resp, !isHead);
            });
        });

        if(!submitted) {
            // 队列满 → 背压：返回 503 Service Unavailable
            // 关键决策：HTTP/1.1 默认长连接，拒绝**不关连接**。
            // 关连接版 503 的代价：客户端重连形成"拒绝→重连→更忙"的自放大
            // 风暴，且服务端作为主动关闭方堆积 TIME_WAIT（实测：-c1000 满负载
            // 时每秒数千次重连 + 服务端 3000+ 个 TIME_WAIT 占用 fd）。
            // 不关连接：拒绝成本恒定，连接留着继续服务后续请求
            HttpResponse resp = HttpResponse::makeError(
                HttpResponse::k503ServiceUnavailable,
                "Server Busy, please retry later"
            );
            Metrics::instance().errors5xx++;
            bool keepAlive = (req.version() == "HTTP/1.1" && !isShutdown);
            if (keepAlive) resp.setCloseConnection(false); // makeError 默认关连接，覆盖掉
            // 503 也是"本请求 seq"的响应，必须走定序——前面可能还有在途响应未发。
            // messageCallback 运行在 IO 线程，可直接同步投递
            if (!keepAlive) conn->markForClose(); // 先标记再投递（同上：direct 写路径唯一评估点是 deliverResponse 末尾）
            conn->deliverResponse(seq, resp, true); // HTTP/1.0 无显式 keep-alive，保持原行为
        }
    });

    // 空闲超时心跳：1s 重复定时器（addTimer 只调一次 timerfd_settime），扫描所有
    // 连接，超过 connectionTimeoutSec 无活动则投递到所属 IO 线程强关。
    // 连接状态（Channel/发送队列）只能在其所属 IO 线程操作，跨线程触达会与
    // IO 线程并发（shutdown() 的 queueInLoop 投递同理）
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = ts.tv_sec * 1'000'000 + ts.tv_nsec / 1'000;
        loop.timerQueue().addTimer([&server, &cfg]() {
            timespec t2;
            clock_gettime(CLOCK_MONOTONIC, &t2);
            int64_t now = t2.tv_sec * 1'000'000 + t2.tv_nsec / 1'000;
            server.forEachConnection([&](const TcpConnection::ptr& conn) {
                if (now - conn->lastActiveTime() >= cfg.connectionTimeoutSec * 1'000'000) {
                    conn->getLoop()->queueInLoop([conn]() { conn->forceClose(); });
                }
            });
        }, now + 1'000'000, 1.0);  // 重复定时器，间隔 1s
    }

    server.setMaxConnections(cfg.maxConnections);
    server.start(cfg.listenBacklog);
    LOG_INFO << "Reactor HTTP server listening on port:" << cfg.port
             << ", IO threads:" << cfg.ioThreads
             << ", worker threads:" << cfg.workerThreads;
    loop.loop();
    // 拆除顺序（两个 ASAN 实测崩溃都出在这三步的顺序上）：
    // 1) threadPool.stop()：quit 所有 worker loop + join 线程。worker 任务里
    //    conn->getLoop()->queueInLoop() 还引用子循环，所以子循环此刻必须还活着。
    //    stop 只 quit+join，loops_ 存活到析构——即使 deadline 兜底时还有活跃
    //    连接、IO 线程继续 tryRun，投递也落进存活队列（永不执行），不变式保持
    // 2) server.shutdown()：子循环 quit+join、连接全部关闭。这一步必须赶在
    //    ~ThreadPool（函数返回后执行）之前——否则还活着的子循环线程会通过
    //    messageCallback → threadPool.tryRun 往即将释放的 pendingFunctors_ 里
    //    投递任务（ASAN 实测 heap-use-after-free WRITE）
    // 3) 函数返回后 ~threadPool 才释放 loops_（关闭 epoll/eventfd fd）——此时所有投递方已死
    threadPool.stop();
    server.shutdown();
    return 0;
}