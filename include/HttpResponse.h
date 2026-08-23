#pragma once
#include <string>
#include <map>
#include <ctime>  // time_t
#include <memory>
#include <unistd.h>  // ::close
#include "Buffer.h"

// 文件响应体 fd 的 RAII 所有权句柄。响应对象在 worker → queueInLoop lambda →
// pendingResp_ 暂存之间多次拷贝，所有副本通过 shared_ptr 共享同一 fd，最后一个
// 副本析构时自动 close——保证恰好关闭一次，既不会在发送完成前被误关，
// 也不会在连接中途死亡（pendingResp_ 未发送条目）时泄漏
struct FileHandle {
    int fd;
    explicit FileHandle(int f) : fd(f) {}
    ~FileHandle() { if (fd >= 0) ::close(fd); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
};
using FileFd = std::shared_ptr<FileHandle>;

// 创建文件 fd 所有权句柄（StaticFileHandler 打开成功后调用）
FileFd makeFileFd(int fd);

class HttpResponse{
public:
    enum HttpStatusCode{ k200Ok = 200, k304NotModified = 304, k400BadRequest = 400, k403Forbidden = 403,
                        k404NotFound = 404, k405MethodNotAllowed = 405,
                        k413PayloadTooLarge = 413, k417ExpectationFailed = 417,
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

    void setFileFd(const FileFd& fd, off_t size); // 设置文件作为响应体（fd 所有权共享，发送完成后自动关闭）
    bool isFileBody() const { return isFileBody_; }; // 判断是否使用文件作为响应体
    const FileFd& fileFd() const { return fileFd_; }; // 文件 fd（TOCTOU 修复后不再按路径二次打开）
    off_t fileBodySize() const { return fileBodySize_; }; // 获取文件大小

private:
    HttpStatusCode statusCode_;// 状态码
    std::string statusMessage_;// 状态消息
    std::string body_;// 主体内容
	std::map<std::string, std::string> headers_;
	bool closeConnection_;
    bool isFileBody_ = false; // 标记是否使用文件作为响应体
    FileFd fileFd_; // 文件 fd 所有权句柄（共享，最后一个引用释放时关闭）
    off_t fileBodySize_ = 0; // 文件大小
};

// time_t → HTTP 日期格式（如 "Fri, 22 Aug 2026 08:00:00 GMT"）。
// 共享实现：RFC 7231 §7.1.1.2 要求所有响应带 Date，序列化时统一生成
std::string httpDate(time_t t);