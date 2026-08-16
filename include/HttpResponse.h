#pragma once
#include <string>
#include <map>
#include "Buffer.h"

class HttpResponse{
public:
    enum HttpStatusCode{ k200Ok = 200, k304NotModified = 304, k400BadRequest = 400, k403Forbidden = 403,
                        k404NotFound = 404, k405MethodNotAllowed = 405,
                        k413PayloadTooLarge = 413,
                        k500InternalServerError = 500, k501NotImplemented = 501,
                        k503ServiceUnavailable = 503,
                        k505HttpVersionNotSupported = 505, };// http状态码枚举

    HttpResponse();// 构造函数
    ~HttpResponse();// 析构函数

    void setStatusCode(HttpStatusCode code);// 设置状态码
    void setStatusMessage(const std::string& msg);// 设置状态消息
    void setBody(const std::string& body);// 设置主体内容
    void addHeader(const std::string& key, const std::string& value);// 添加头部
    void setCloseConnection(bool on);// 设置关闭连接标识
    bool closeConnection() const;// 获取是否关闭连接
    const std::string& body() const;//获取body数据
    const HttpStatusCode& code() const;//获取状态码
    const std::string& msg() const;//获取状态信息
    const std::string& header_value(const std::string& key) const;//获取header对应value 
    static HttpResponse makeError(HttpStatusCode code, const std::string& message);// 创建错误响应对象

    void appendToBuffer(Buffer* buf) const;// 序列化成 HTTP 响应报文
    std::string toString(bool includeBody = true) const;// 序列化完整响应；includeBody=false 时只含头部（HEAD 请求用）
    std::string headersToString() const; // 将响应头部转换为字符串形式

    void setFileBody(const std::string& filepath, off_t size); // 设置文件作为响应体
    bool isFileBody() const { return isFileBody_; }; // 判断是否使用文件作为响应体
    const std::string& fileBodyPath() const { return fileBodyPath_; }; // 获取文件路径
    off_t fileBodySize() const { return fileBodySize_; }; // 获取文件大小
    off_t fileBodyOffset() const { return fileBodyOffset_; }; // 获取文件偏移量

private:
    HttpStatusCode statusCode_;// 状态码
    std::string statusMessage_;// 状态消息
    std::string body_;// 主体内容
	std::map<std::string, std::string> headers_;
	bool closeConnection_;
    bool isFileBody_ = false; // 标记是否使用文件作为响应体
    std::string fileBodyPath_; // 文件路径
    off_t fileBodySize_ = 0; // 文件大小
    off_t fileBodyOffset_ = 0; // 文件偏移量
};