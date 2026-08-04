#include "HttpRequest.h"

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
    auto it = headers_.find(key);
    return it->second;
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