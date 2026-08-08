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

int main(int argc, char* argv[]) {
    ServerConfig cfg;
    ConfigParser parser;
    if (!parser.parse(argc, argv, cfg)) {
        return 0;
    }

    Logger::setLevel(LogLevel(Logger::parseLogLevel(cfg.logLevel)));
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



    auto resetTimer = [&cfg](TcpConnection::ptr conn) {
        if (conn->timerId() != 0) {
            conn->getLoop()->timerQueue().cancel(conn->timerId());
            conn->setTimerId(0);
        }

        // 设置新定时器：expiration 是绝对时间 (CLOCK_MONOTONIC 微秒)
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = ts.tv_sec * 1'000'000 + ts.tv_nsec / 1'000;
        int64_t expiration = now + cfg.connectionTimeoutSec * 1'000'000;

        int64_t timerId = conn->getLoop()->timerQueue().addTimer([conn]() {
            conn->forceClose();
        }, expiration);

        conn->setTimerId(timerId);
    };

    server.setConnectionCallback([&resetTimer](TcpConnection::ptr conn) {
        resetTimer(conn);
        conn->setCloseCallback([](TcpConnection::ptr c) {
            if (c->timerId() != 0) {
                c->getLoop()->timerQueue().cancel(c->timerId());
                c->setTimerId(0);
            }
        });
    });

    server.setMessageCallback([&resetTimer, &threadPool, &router, &staticHandler, &isShutdown]
                              (TcpConnection::ptr conn, Buffer* buf) {
        // 排空期（SIGTERM 后）：请求照常处理，但响应后连接即关闭。
        // 不能静默丢弃——客户端已发出的请求必须得到响应，否则挂起直到超时
        // （实测：keep-alive 连接在 shutdown 后发请求，无响应挂 3s）。
        // markForClose 在发送队列清空后自动关连接，排空自然收敛
        if (isShutdown) conn->markForClose();

        HttpContext& ctx = conn->context();

        // 解析请求（解析状态累积在 ctx 内部，TCP 拆包时多次 EPOLLIN 之间不丢失）
        if (!ctx.parseRequest(buf)) {
            if (ctx.error() == HttpContext::kNoError) return; // 请求不完整，继续等待
            else if (ctx.error() == HttpContext::kBadRequest) { // 400 Bad Request
                Metrics::instance().errors4xx++;
                conn->send(HttpResponse::makeError(
                    HttpResponse::k400BadRequest, "Bad Request").toString());
                conn->markForClose();
                ctx.reset();
                return;
            } else if (ctx.error() == HttpContext::kMethodNotSupported) { // 405 Method Not Allowed
                Metrics::instance().errors4xx++;
                conn->send(HttpResponse::makeError(
                    HttpResponse::k405MethodNotAllowed,
                    "Method Not Supported").toString());
                conn->markForClose();
                ctx.reset();
                return;
            } else if (ctx.error() == HttpContext::kVersionNotSupported) { // 505 HTTP Version Not Supported
                Metrics::instance().errors5xx++;
                conn->send(HttpResponse::makeError(
                    HttpResponse::k505HttpVersionNotSupported,
                    "Http Version Not Supported").toString());
                conn->markForClose();
                ctx.reset();
                return;
            } else if (ctx.error() == HttpContext::kHeaderTooLarge) { // 413 Header Too Large
                Metrics::instance().errors4xx++;
                conn->send(HttpResponse::makeError(
                    HttpResponse::k413PayloadTooLarge,
                    "Request Header Too Large").toString());
                conn->markForClose();
                ctx.reset();
                return;
            }
        }

        // 取出解析结果，处理完复位，等待下一个请求
        HttpRequest req = ctx.request();
        ctx.reset();
        Metrics::instance().totalRequests++;
        resetTimer(conn);

        bool submitted = threadPool.tryRun([conn, req, &router, &staticHandler, &isShutdown]() {
            HttpResponse resp;

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
            conn->getLoop()->queueInLoop([conn, resp, closeConn, isHead]() {
                if(resp.isFileBody()){
                    conn->sendResponse(resp, !isHead);
                } else {
                    conn->send(resp.toString(!isHead));
                }
                if (closeConn) conn->markForClose();
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
            conn->getLoop()->queueInLoop([conn, resp, keepAlive]() {
                conn->send(resp.toString());
                if (!keepAlive) conn->markForClose(); // HTTP/1.0 无显式 keep-alive，保持原行为
            });
        }
    });

    server.setMaxConnections(cfg.maxConnections);
    server.start(cfg.listenBacklog);
    LOG_INFO << "Reactor HTTP server listening on port:" << cfg.port
             << ", IO threads:" << cfg.ioThreads
             << ", worker threads:" << cfg.workerThreads;
    loop.loop();
    // 拆除顺序（两个 ASAN 实测崩溃都出在这三步的顺序上）：
    // 1) threadPool.stop()：join 所有 worker。worker 任务里 conn->getLoop()->
    //    queueInLoop() 还引用子循环，所以子循环此刻必须还活着
    // 2) server.shutdown()：子循环 quit+join、连接全部关闭。这一步必须赶在
    //    ~ThreadPool（函数返回后执行）之前——否则还活着的子循环线程会通过
    //    messageCallback → threadPool.tryRun 往即将释放的 tasks_ deque 里
    //    投递任务（ASAN 实测 heap-use-after-free WRITE，ThreadPool.cpp:35）
    // 3) 函数返回后 ~threadPool 才真正释放任务队列——此时所有投递方已死
    threadPool.stop();
    server.shutdown();
    return 0;
}