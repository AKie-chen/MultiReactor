#pragma once
#include "Buffer.h"
#include "HttpRequest.h"

// 解析http请求
class HttpContext{
public:
    enum ParseState { kExpectRequestLine, kExpectHeaders, kExpectBody, KGotCompleteRequest }; // 解析状态
    enum ParseError { kNoError, kBadRequest, kMethodNotSupported, kVersionNotSupported, kHeaderTooLarge };  // 解析错误类型

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

private:
    bool parseRequestLine(std::string& line, HttpRequest* req);
    bool parseHeader(std::string& line, HttpRequest* req);
    int findCrlf(Buffer* buf, const char* cl, std::string& line);

    ParseState state_;
    ParseError error_;
    size_t contentLength_;//从Content_Length 头部解析出的body长度
    size_t headerBytes_ = 0;  // 已解析头部累计字节数（跨多次 EPOLLIN 累计）
    HttpRequest request_; // 跨多次EPOLLIN累积解析状态（TCP拆包时请求行信息不能丢）
};