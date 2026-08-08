#include "HttpRequest.h"
#include <cstring>
#include <strings.h>  // strcasecmp

void HttpRequest::setMethod(Method m)//设置方法
{
    method_ = m;
}

HttpRequest::Method HttpRequest::method() const//获取方法
{
    return method_;
}

void HttpRequest::setPath(const std::string& path)//设置url
{
    path_ = path;
}

const std::string& HttpRequest::path() const//获取url
{
    return path_;
}

void HttpRequest::setVersion(const std::string& ver)//设置http版本，1/1.1
{
    version_ = ver;
}

const std::string& HttpRequest::version() const//获取版本
{
    return version_;
}

void HttpRequest::addHeader(const std::string& key, const std::string& value)//添加头
{
    headers_[key] = value;
}

std::string HttpRequest::getHeader(const std::string& key) const//获取头
{
    // HTTP 头名大小写不敏感（RFC 7230 §3.2）：客户端可能发 Connection/connection/CONNECTION。
    // 精确 map 查找会漏掉变体；头数量少（通常 < 20），线性扫描成本可忽略
    for (const auto& [k, v] : headers_) {
        if (strcasecmp(k.c_str(), key.c_str()) == 0) return v;
    }
    // 找不到返回空串：直接解引用 end() 是未定义行为，
    // 会从垃圾指针构造 string 导致 bad_alloc 崩溃（If-Modified-Since 场景实测）
    return std::string();
}

const std::map<std::string, std::string>& HttpRequest::headers() const//获取头部哈希
{
    return headers_;
}

void HttpRequest::setBody(const std::string& body)//设置数据
{
    body_ = body;
}

const std::string& HttpRequest::body() const//获取数据
{
    return body_;
}

void HttpRequest::setQuery(const std::string& query)//设置查询参数
{
    query_ = query;
}

const std::string& HttpRequest::query() const//获取查询参数
{
    return query_;
}

void HttpRequest::setRawPath(const std::string& raw)//设置原始路径
{
    rawPath_ = raw;
}

const std::string& HttpRequest::rawPath() const//获取原始路径
{
    return rawPath_;
}