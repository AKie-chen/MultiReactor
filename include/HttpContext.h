#pragma once
#include "Buffer.h"
#include "HttpRequest.h"

// 解析http请求
class HttpContext{
public:
    enum ParseState { kExpectRequestLine, kExpectHeaders, kExpectBody, KGotCompleteRequest }; // 解析状态
    enum ParseError { kNoError, kBadRequest, kMethodNotSupported, kVersionNotSupported, kHeaderTooLarge, kNotImplemented, kExpectationFailed };  // 解析错误类型

    // 头部大小上限（防止无界内存消耗）：单行 8KB、累计 64KB，超出 → 413
    static constexpr size_t kMaxHeaderLine = 8 * 1024;
    static constexpr size_t kMaxHeaderBytes = 64 * 1024;
    // 请求体大小上限（防内存 DoS：Content-Length 可声明任意大小），超出 → 413
    static constexpr size_t kMaxBodyBytes = 16 * 1024 * 1024;

    HttpContext();

    //返回true = 完整请求解析完成（数据在 request() 中）
    //返回false = 数据不够，等待更多数据（下次EPOLLIN继续）
    //注意：请求可能跨多次EPOLLIN到达（TCP拆包），解析状态保存在内部，
    //      HttpRequest 必须由 HttpContext 持有，不能是调用方的局部变量
    bool parseRequest(Buffer* buf);

    const HttpRequest& request() const { return request_; }  // 解析完成的请求

    void reset();//一个请求处理完，复位等待下一个

    //返回解析状态
    ParseState state() const { return state_; }
    ParseError error() const { return error_; }

    // RFC 7231 §5.1.1：请求带 Expect: 100-continue 且头部已解析完（等待 body）→
    // 应回 100 Continue。仅当进入 kExpectBody（确实有 body 要收）且未发过时返回 true
    bool shouldSendContinue() const { return expectContinue_ && !continueSent_ && state_ == kExpectBody; }
    void markContinueSent() { continueSent_ = true; }  // 100 Continue 已发送（每个请求最多一次）

private:
    bool parseRequestLine(std::string& line, HttpRequest* req);
    bool parseHeader(std::string& line, HttpRequest* req);
    int findCrlf(Buffer* buf, const char* cl, std::string& line);

    ParseState state_;
    ParseError error_;
    size_t contentLength_;//从Content_Length 头部解析出的body长度
    bool contentLengthSeen_ = false;  // 是否已收到 Content-Length（重复头检查, RFC 7230 §3.3.2）
    bool hostSeen_ = false;  // 是否已收到 Host（重复头检查, RFC 7230 §3.2.2）
    bool expectContinue_ = false;  // 收到 Expect: 100-continue（RFC 7231 §5.1.1）
    bool continueSent_ = false;  // 本请求的 100 Continue 已发出（只回一次）
    size_t headerBytes_ = 0;  // 已解析头部累计字节数（跨多次 EPOLLIN 累计）
    HttpRequest request_; // 跨多次EPOLLIN累积解析状态（TCP拆包时请求行信息不能丢）
};