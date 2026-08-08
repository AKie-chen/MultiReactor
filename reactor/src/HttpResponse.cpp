#include "HttpResponse.h"

HttpResponse::HttpResponse() : statusCode_(HttpStatusCode::k200Ok)
                             , statusMessage_("OK")
                             , body_()
                             , headers_()
                             , closeConnection_(false) {}// 构造函数{}// 构造函数

HttpResponse::~HttpResponse(){}// 析构函数

void HttpResponse::setStatusCode(HttpStatusCode code)// 设置状态码
{
    statusCode_ = code;
}

void HttpResponse::setStatusMessage(const std::string& msg)// 设置状态消息
{
    statusMessage_ = msg;
}

void HttpResponse::setBody(const std::string& body)// 设置主体内容
{
    body_ = body;
}

void HttpResponse::addHeader(const std::string& key, const std::string& value)// 添加头部
{
    headers_[key] = value;
}

void HttpResponse::setCloseConnection(bool on)// 设置关闭连接标识
{
    closeConnection_ = on;
}

bool HttpResponse::closeConnection() const// 获取是否关闭连接
{
    return closeConnection_;
}

const std::string& HttpResponse::body() const//获取body数据
{
    return body_;
}

const HttpResponse::HttpStatusCode& HttpResponse::code() const//获取状态码
{
    return statusCode_;
}

const std::string& HttpResponse::msg() const//获取状态信息
{
    return statusMessage_;
}

const std::string& HttpResponse::header_value(const std::string& key) const//获取header对应value 
{
    auto it = headers_.find(key);
    static const std::string empty;
    return it != headers_.end() ? it->second : empty;
}

void HttpResponse::appendToBuffer(Buffer* buf) const// 序列化成 HTTP 响应报文
{   
    //序列化行
    std::string statusLine = "HTTP/1.1 " + std::to_string(statusCode_) + " " +statusMessage_ + "\r\n";
    buf->append(statusLine.data(), statusLine.size());

    //序列化头
    for(auto& [key, value] : headers_){
        std::string h = key + ": " + value + "\r\n";
        buf->append(h.data(), h.size());
    }

    //空行
    buf->append("\r\n", 2);
    
    //序列化Body
    if(!isFileBody_){
        buf->append(body_.data(), body_.size());
    }
}

std::string HttpResponse::toString(bool includeBody) const// 将响应对象转换为字符串形式
{
    std::string result = headersToString();
    // HEAD 请求只允许返回头部（RFC 7231 §4.3.2）：Content-Length 保留真实大小，但不发 body
    if(includeBody && !isFileBody_){
        result += body_;
    }

    return result;
}

std::string HttpResponse::headersToString() const // 将响应头部转换为字符串形式
{
    std::string result;
    // 预估大小，减少 realloc
    size_t estimate = 64 + statusMessage_.size();
    for (auto& [k, v] : headers_) estimate += k.size() + v.size() + 4;
    result.reserve(estimate);

    // 状态行
    result += "HTTP/1.1 " + std::to_string(statusCode_) + " " + statusMessage_ + "\r\n";
    // 头部
    for (auto& [k, v] : headers_) {
        result += k + ": " + v + "\r\n";
    }
    // HTTP/1.0 短连接或错误响应：发送后关闭连接
    if (closeConnection_) {
        result += "Connection: close\r\n";
    }
    // 空行
    result += "\r\n";

    return result;
}

HttpResponse HttpResponse::makeError(HttpResponse::HttpStatusCode code, const std::string& message)// 创建错误响应对象
{
    std::string body = "<html><body><h1>" + std::to_string(code) + " " + message + "</h1></body></html>";

    HttpResponse response;
    response.statusCode_ = code;
    response.statusMessage_ = message;
    response.body_ = body;
    response.headers_["Content-Length"] = std::to_string(body.size());
    response.headers_["Content-Type"] = "text/html";
    response.closeConnection_ = true; // 错误响应通常会关闭连接

    return response;
}

void HttpResponse::setFileBody(const std::string& filepath, off_t size) // 设置文件作为响应体
{
    isFileBody_ = true;
    fileBodyPath_ = filepath;
    fileBodySize_ = size;
}