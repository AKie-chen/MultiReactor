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
    ThreadPool threadPool(cfg.workerThreads);

    SignalHandler signalHandler(&loop);
    signalHandler.addSignal(SIGINT);
    signalHandler.addSignal(SIGTERM);
    signalHandler.setShutdownCallback([&loop, &server]() {
        LOG_INFO << "Shutdown signal received, stopping server...";
        server.shutdown();
        loop.quit();
    });

    Router router;
    StaticFileHandler staticHandler(cfg.staticDir, cfg.maxFileSizeMB * 1024 * 1024);

    router.addRoute(HttpRequest::kGet, "/", [](const HttpRequest& req, HttpResponse* resp) {
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setBody("Hello, World!");
        resp->addHeader("Content-Length", std::to_string(resp->body().size()));
        resp->addHeader("Content-Type", "text/plain");
    });

    router.addRoute(HttpRequest::kGet, "/stats", [](const HttpRequest&, HttpResponse* resp) {
        std::string json = Metrics::instance().toJson();
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setBody(json);
        resp->addHeader("Content-Type", "application/json");
        resp->addHeader("Content-Length", std::to_string(json.size()));
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

    server.setMessageCallback([&resetTimer, &threadPool, &router, &staticHandler]
                              (TcpConnection::ptr conn, Buffer* buf) {
        HttpContext& ctx = conn->context();

        // 解析请求（解析状态累积在 ctx 内部，TCP 拆包时多次 EPOLLIN 之间不丢失）
        if (!ctx.parseRequest(buf)) {
            if (ctx.error() == HttpContext::kNoError) return; // 请求不完整，继续等待
            else if (ctx.error() == HttpContext::kBadRequest) { // 400 Bad Request
                Metrics::instance().errors4xx++;
                conn->send(HttpResponse::makeError(
                    HttpResponse::k400BadRequest, "Bad Request").toString());
                conn->markForClose();
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
            }
        }

        // 取出解析结果，处理完复位，等待下一个请求
        HttpRequest req = ctx.request();
        ctx.reset();
        Metrics::instance().totalRequests++;
        resetTimer(conn);

        bool submitted = threadPool.tryRun([conn, req, &router, &staticHandler]() {
            HttpResponse resp;
            if (!router.route(req, &resp)) {
                if (!staticHandler.handle(req, &resp)) {
                    resp = HttpResponse::makeError(
                        HttpResponse::k404NotFound, "Not Found");
                }
            }
            int code = static_cast<int>(resp.code());
            if (code >= 400 && code < 500) Metrics::instance().errors4xx++;
            else if (code >= 500) Metrics::instance().errors5xx++;

            // HTTP/1.0 默认短连接：响应发送完毕后关闭连接
            bool closeConn = resp.closeConnection();
            if (req.version() == "HTTP/1.0" && !closeConn) {
                closeConn = true;
                resp.setCloseConnection(true);
            }

            conn->getLoop()->queueInLoop([conn, resp, closeConn]() {
                if(resp.isFileBody()){
                    conn->sendResponse(resp);
                } else {
                    conn->send(resp.toString());
                }
                if (closeConn) conn->markForClose();
            });
        });

        if(!submitted) {
            // 队列满 → 主线程直接处理，或返回 503
            HttpResponse resp = HttpResponse::makeError(
                HttpResponse::k500InternalServerError,  // 或自定义 503
                "Server busy, please retry later"
            );
            conn->getLoop()->queueInLoop([conn, data = resp.toString()]() {
            conn->send(data);
            });
        }
    });

    server.setMaxConnections(cfg.maxConnections);
    server.start(cfg.listenBacklog);
    LOG_INFO << "Reactor HTTP server listening on port:" << cfg.port
             << ", IO threads:" << cfg.ioThreads
             << ", worker threads:" << cfg.workerThreads;
    loop.loop();
    return 0;
}