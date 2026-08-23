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
// plusToSpace：仅 query 为 true（application/x-www-form-urlencoded 惯例，RFC 3986 §2.3
// 之外的表单扩展）。path 中 + 是合法 pchar（sub-delims），必须保持字面量
static std::string urlDecode(const std::string& src, bool plusToSpace)
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
        } else if (src[i] == '+' && plusToSpace) {
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

        // RFC 7230 §5.4：HTTP/1.1 请求必须带 Host，缺失 → 400（HTTP/1.0 不强制，RFC 1945）
        if (request_.version() == "HTTP/1.1" && !hostSeen_) {
            error_ = kBadRequest;
            return false;
        }

        // 声明体长超限 → 立即 413，不必等 body 字节（Content-Length 可声明任意大小，
        // 不提前拦截会无界吃内存（DoS））。放在状态转移前对 Expect: 100-continue 尤为关键：
        // 必须先回最终状态码，绝不能先发 100 Continue 再反悔
        if (contentLength_ > kMaxBodyBytes) {
            error_ = kHeaderTooLarge;
            return false;
        }

        state_ = (contentLength_ > 0) ? kExpectBody : KGotCompleteRequest;
    }

    if(state_ == kExpectBody){
        if (buf->readableBytes() < contentLength_) {
            return false;  // 数据不够，等待更多数据
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
    contentLengthSeen_ = false;
    hostSeen_ = false;
    expectContinue_ = false;
    continueSent_ = false;
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
            // query 才做 application/x-www-form-urlencoded 的 + → 空格（RFC 3986 §2.3
            // 之外的表单惯例）；%xx 解码与 path 相同
            req->setQuery(urlDecode(path.substr(queryPos + 1), true));
            req->setPath(urlDecode(path.substr(0, queryPos), false));  // path 中 + 保持字面量（RFC 3986）
        } else {
            req->setQuery("");
            req->setPath(urlDecode(path, false));
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
    // RFC 7230 §3.3.1：不支持的 Transfer-Encoding → 501，绝不能静默忽略。
    // 若把 chunked 请求当无 body 处理，chunked 帧剩余字节会被当新请求解析，
    // 产生连锁 400 与响应错配（实测）；前置代理按 TE 解析时即为走私向量
    if (strcasecmp(key.c_str(), "Transfer-Encoding") == 0) {
        error_ = kNotImplemented;
        return false;
    }
    // 头名大小写不敏感（RFC 7230 §3.2）：content-length: 也必须识别，
    // 否则小写变体下 contentLength_ 恒为 0，body 被当新请求行解析（错位）
    if (strcasecmp(key.c_str(), "Content-Length") == 0){
        // 必须全数字：stoul 前缀解析会把 "5abc" 当 5（请求走私风险）
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
            error_ = kBadRequest;
            return false;
        }
        size_t len;
        try{
            len = std::stoul(value);
        }catch(const std::exception& e){
            error_ = kBadRequest;
            return false;
        }
        // RFC 7230 §3.3.2：重复 Content-Length 值冲突 → 400（走私向量：
        // 前置代理取第一个值、本服务器取最后一个值）。同值重复可容忍
        if (contentLengthSeen_) {
            if (len != contentLength_) {
                error_ = kBadRequest;
                return false;
            }
        } else {
            contentLength_ = len;
            contentLengthSeen_ = true;
        }
    }

    // RFC 7230 §5.4 Host 头必须存在，否则 400
    if (strcasecmp(key.c_str(), "Host") == 0) {
        // 必须有值
        if (value.empty()) {
            error_ = kBadRequest;
            return false;
        }
        // 第二个 Host 头冲突 → 400
        if (hostSeen_){
            error_ = kBadRequest;
            return false;
        }
        hostSeen_ = true;
    }

    // RFC 7231 §5.1.1：Expect: 100-continue → 解析完头部后回 100 Continue；
    // 其他 Expect 值无法满足 → 417，绝不能静默忽略——否则客户端会一直等 100 再发 body
    if (strcasecmp(key.c_str(), "Expect") == 0) {
        // 值允许带 OWS（RFC 7230 §3.2.4），比较前修剪首尾空白
        std::string v = value;
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        if (strcasecmp(v.c_str(), "100-continue") == 0) {
            expectContinue_ = true;
        } else {
            error_ = kExpectationFailed;
            return false;
        }
    }

    return true;
}