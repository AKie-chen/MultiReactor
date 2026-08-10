#include "HttpContext.h"
#include<algorithm>
#include<cstring>
#include<strings.h>  // strcasecmp

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1; // Invalid hex character
}

// url解码函数，将%xx转为对应字符
static std::string urlDecode(const std::string& src)
{
    std::string dest;
    dest.clear();
    dest.reserve(src.size());

    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '%') {
            if (i + 2 < src.size()) {
                int hi = hexVal(src[i + 1]);
                int lo = hexVal(src[i + 2]);
                if (hi >= 0 && lo >= 0) {   // 非法编码（%ZZ/%2Z）必须原样保留，不能输出垃圾字节
                    dest += static_cast<char>((hi << 4) | lo);
                    i += 2;
                } else {
                    dest += '%';
                }
            } else {
                dest += '%';
            }
        } else if (src[i] == '+') {
            dest += ' ';
        } else {
            dest += src[i];
        }
    }

    return dest;
}

HttpContext::HttpContext() : state_(kExpectRequestLine)
                           , error_(kNoError)
                           , contentLength_(0) {}

int HttpContext::findCrlf(Buffer* buf, const char* cl ,std::string& line)//查找请求行/请求头
{
    const char* begin = buf->peek();
    const char* end = begin + buf->readableBytes();

    auto crlf = std::search(begin, end, cl, cl + strlen(cl));

    if (crlf == end) return -1;  // 没找到完整数据，等更多数据

    line.assign(begin, crlf);         // 不含cl的行内容
    buf->retrieve((crlf - begin) + strlen(cl));     // ← 在这里 retrieve！消费 行内容cl

    return 0;
}

//返回true = 完整请求解析完成（数据在 request_ 中）
//返回false = 数据不够，等待更多数据（下次EPOLLIN继续）
//解析结果累积到内部 request_ 成员，跨多次调用保留（TCP拆包时请求行信息不丢失）
bool HttpContext::parseRequest(Buffer* buf)
{
    if(state_ == kExpectRequestLine){
        std::string line;
        if(findCrlf(buf,"\r\n", line) < 0) return false;//数据不够

        if (line.size() > kMaxHeaderLine) {  // 请求行过长（超长 URI 等）
            error_ = kHeaderTooLarge;
            return false;
        }
        if (!parseRequestLine(line, &request_)) return false;  // 格式错误

        state_ = kExpectHeaders;
    }

    if(state_ == kExpectHeaders){
        while(1){
            std::string line;
            if(findCrlf(buf,"\r\n", line) < 0) return false;
            if(line.empty()) break;
            headerBytes_ += line.size();
            if (line.size() > kMaxHeaderLine || headerBytes_ > kMaxHeaderBytes) {
                error_ = kHeaderTooLarge;  // 防 DoS：无界头部会无限吃内存
                return false;
            }
            if(!parseHeader(line, &request_)) return false;
        }

        state_ = (contentLength_ > 0) ? kExpectBody : KGotCompleteRequest;
    }

    if(state_ == kExpectBody){
        if (buf->readableBytes() < contentLength_) {
            // 数据未到齐也要先检查声明长度：Content-Length 可声明任意大小，
            // 不提前拦截会无界吃内存（DoS）。声明超限 → 413，不必等数据到齐
            if (contentLength_ > kMaxBodyBytes) {
                error_ = kHeaderTooLarge;
                return false;
            }
            return false;  // 数据不够
        }
        request_.setBody(buf->retrieve(contentLength_));
        state_ = KGotCompleteRequest;
    }

    return true;
}

void HttpContext::reset()//一个请求处理完，复位等待下一个
{
    state_ = kExpectRequestLine;
    contentLength_ = 0;
    headerBytes_ = 0;
    error_ = kNoError;
    request_ = HttpRequest();  // 清空上一个请求的解析数据
}

bool HttpContext::parseRequestLine(std::string& line, HttpRequest* req)
{
    size_t sp1 = line.find(' ', 0);//第一个空格位置
    size_t sp2 = line.find(' ', sp1 + 1);//第二个空格位置
    if(sp1 == std::string::npos || sp2 == std::string::npos) {
        error_ = kBadRequest;
        return false;
    }

    std::string method = line.substr(0, sp1);//设置方法
    if (method == "GET") req->setMethod(HttpRequest::kGet);
    else if (method == "POST") req->setMethod(HttpRequest::kPost);
    else if (method == "HEAD") req->setMethod(HttpRequest::kHead);
    else {
        error_ = kMethodNotSupported;
        return false;
    }

    std::string path = line.substr(sp1 + 1, sp2 - sp1 - 1);//设置路径
    if(!path.empty()) {
        req->setRawPath(path);  // 原始路径（解码前）

        size_t queryPos = path.find('?');
        if (queryPos != std::string::npos) {
            req->setQuery(path.substr(queryPos + 1));
            req->setPath(urlDecode(path.substr(0, queryPos)));
        } else {
            req->setQuery("");
            req->setPath(urlDecode(path));
        }
    }else{
        error_ = kBadRequest;
        return false;
    }

    std::string version = line.substr(sp2 + 1);//设置版本
    if(version == "HTTP/1.1" || version == "HTTP/1.0") {
        req->setVersion(version);
    } else {
        error_ = kVersionNotSupported;
        return false;
    }
    
    return true;
}

bool HttpContext::parseHeader(std::string& line, HttpRequest* req)
{
    size_t colon = line.find(':');//一行头为Host: localhost,找":"
    if(colon == std::string::npos) {
        error_ = kBadRequest;
        return false;
    }
    std::string key = line.substr(0,colon);
    // 头名尾部空白 trim（RFC 7230 §3.2.4）："Connection : close" 的 key 是 "Connection "
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    size_t valBegin = colon + 1;

    if (valBegin < line.size() && line[valBegin] == ' ') valBegin++;  // 跳过 ": " 的空格
    std::string value = line.substr(valBegin);

    req->addHeader(key, value);
    // 头名大小写不敏感（RFC 7230 §3.2）：content-length: 也必须识别，
    // 否则小写变体下 contentLength_ 恒为 0，body 被当新请求行解析（错位）
    if (strcasecmp(key.c_str(), "Content-Length") == 0){
        // 必须全数字：stoul 前缀解析会把 "5abc" 当 5（请求走私风险）
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
            error_ = kBadRequest;
            return false;
        }
        try{
            contentLength_ = std::stoul(value);
        }catch(const std::exception& e){
            error_ = kBadRequest;
            return false;
        }
    }


    return true;
}