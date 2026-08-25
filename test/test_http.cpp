#include "test_framework.h"
#include "HttpContext.h"
#include "HttpResponse.h"
#include "Buffer.h"
#include <algorithm>
#include <string>
#include <ctime>

// ---------- 辅助 ----------

// 一次性喂入完整请求（HTTP 报文完整到达的场景）
static HttpContext parseOnce(const std::string& raw) {
    HttpContext ctx;
    Buffer buf;
    buf.append(raw.data(), raw.size());
    ctx.parseRequest(&buf);
    return ctx;
}

// 拆包：按 chunkSize 逐块喂入，覆盖 TCP 任意分包场景
// （网络层每次只给一小块，parseRequest 需跨多次调用累积解析状态）
static HttpContext parseChunked(const std::string& raw, size_t chunkSize) {
    HttpContext ctx;
    Buffer buf;
    for (size_t i = 0; i < raw.size(); i += chunkSize) {
        size_t n = std::min(chunkSize, raw.size() - i);
        buf.append(raw.data() + i, n);
        ctx.parseRequest(&buf);
    }
    return ctx;
}

// ---------- 基本解析 ----------

TEST_CASE(BasicGet) {
    HttpContext ctx = parseOnce("GET /user?id=1&q=a+b HTTP/1.1\r\n"
                                "Host: example.com\r\n"
                                "User-Agent: test\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kNoError);
    CHECK_EQ(ctx.state(), HttpContext::KGotCompleteRequest);
    CHECK_EQ(ctx.request().method(), HttpRequest::kGet);
    CHECK_EQ(ctx.request().path(), "/user");
    CHECK_EQ(ctx.request().query(), "id=1&q=a b");  // query 的 + → 空格（表单惯例，RFC 3986 之外）
    CHECK_EQ(ctx.request().version(), "HTTP/1.1");
    CHECK_EQ(ctx.request().getHeader("Host"), "example.com");
    CHECK(ctx.request().body().empty());
}

TEST_CASE(PostWithBody) {
    HttpContext ctx = parseOnce("POST /a HTTP/1.1\r\n"
                                "Host: x\r\n"
                                "Content-Length: 5\r\n\r\n"
                                "hello");
    CHECK_EQ(ctx.error(), HttpContext::kNoError);
    CHECK_EQ(ctx.state(), HttpContext::KGotCompleteRequest);
    CHECK_EQ(ctx.request().method(), HttpRequest::kPost);
    CHECK_EQ(ctx.request().body(), "hello");
}

TEST_CASE(HeadMethod) {
    HttpContext ctx = parseOnce("HEAD /a HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kNoError);
    CHECK_EQ(ctx.request().method(), HttpRequest::kHead);
}

// ---------- 拆包（TCP 分包） ----------

TEST_CASE(ByteByByteFragmentation) {
    const std::string raw = "GET /frag HTTP/1.1\r\nHost: x\r\nX-A: 1\r\n\r\n";
    for (size_t chunk : {size_t(1), size_t(3), size_t(7), size_t(64)}) {
        HttpContext ctx = parseChunked(raw, chunk);
        CHECK_EQ(ctx.error(), HttpContext::kNoError);
        CHECK_EQ(ctx.state(), HttpContext::KGotCompleteRequest);
        CHECK_EQ(ctx.request().path(), "/frag");
    }
}

TEST_CASE(BodySplitAcrossChunks) {
    const std::string body = "0123456789";  // 10 字节 body
    std::string raw = "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n" + body;
    for (size_t chunk : {size_t(1), size_t(6), size_t(10), size_t(4)}) {
        HttpContext ctx = parseChunked(raw, chunk);
        CHECK_EQ(ctx.error(), HttpContext::kNoError);
        CHECK_EQ(ctx.state(), HttpContext::KGotCompleteRequest);
        CHECK_EQ(ctx.request().body(), body);
    }
}

TEST_CASE(IncompleteDoesNotConsume) {
    // 只喂到请求行 + 半个头部：parseRequest 不能消费不完整的数据，
    // 否则下一块到达时数据已丢（请求行解析错误）
    HttpContext ctx;
    Buffer buf;
    std::string partial = "GET /a HTTP/1.1\r\nHo";
    buf.append(partial.data(), partial.size());
    CHECK(!ctx.parseRequest(&buf));
    CHECK_EQ(ctx.error(), HttpContext::kNoError);       // 不完整 ≠ 错误
    CHECK_EQ(buf.readableBytes(), size_t(2));           // "Ho" 未消费
    std::string rest = "st: x\r\n\r\n";
    buf.append(rest.data(), rest.size());
    CHECK(ctx.parseRequest(&buf));
    CHECK_EQ(ctx.request().path(), "/a");
    CHECK_EQ(ctx.request().getHeader("Host"), "x");
}

// ---------- 畸形请求行 ----------

TEST_CASE(MalformedRequestLine) {
    HttpContext ctx = parseOnce("GET\r\n\r\n");                 // 无空格 → 无方法/路径/版本
    CHECK_EQ(ctx.error(), HttpContext::kBadRequest);
    ctx = parseOnce("GET /a\r\n\r\n");                          // 缺版本
    CHECK_EQ(ctx.error(), HttpContext::kBadRequest);
    ctx = parseOnce("GET  HTTP/1.1\r\nHost: x\r\n\r\n");        // 空路径
    CHECK_EQ(ctx.error(), HttpContext::kBadRequest);
}

TEST_CASE(MethodNotSupported) {
    HttpContext ctx = parseOnce("PUT /a HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kMethodNotSupported);    // → 405
}

TEST_CASE(VersionNotSupported) {
    HttpContext ctx = parseOnce("GET /a HTTP/2.0\r\nHost: x\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kVersionNotSupported);   // → 505
}

// ---------- Host 头（RFC 7230 §5.4） ----------

TEST_CASE(MissingHostHTTP11) {
    HttpContext ctx = parseOnce("GET /a HTTP/1.1\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kBadRequest);            // HTTP/1.1 必须带 Host → 400
}

TEST_CASE(NoHostRequiredHTTP10) {
    HttpContext ctx = parseOnce("GET /a HTTP/1.0\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kNoError);               // HTTP/1.0 不强制（RFC 1945）
    CHECK_EQ(ctx.state(), HttpContext::KGotCompleteRequest);
}

TEST_CASE(EmptyHostValue) {
    HttpContext ctx = parseOnce("GET /a HTTP/1.1\r\nHost:\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kBadRequest);
}

TEST_CASE(DuplicateHostConflict) {
    HttpContext ctx = parseOnce("GET /a HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kBadRequest);            // 第二个 Host → 400
}

TEST_CASE(LowercaseHostHeader) {
    // 头名大小写不敏感：小写 host 也要算"Host 已提供"
    HttpContext ctx = parseOnce("GET /a HTTP/1.1\r\nhost: x\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kNoError);
    CHECK_EQ(ctx.request().getHeader("HOST"), "x");             // 读取同样大小写不敏感
}

// ---------- Content-Length（RFC 7230 §3.3.2） ----------

TEST_CASE(ConflictingContentLength) {
    HttpContext ctx = parseOnce("POST /a HTTP/1.1\r\nHost: x\r\n"
                                "Content-Length: 5\r\nContent-Length: 6\r\n\r\nhello");
    CHECK_EQ(ctx.error(), HttpContext::kBadRequest);            // CL 冲突 → 400（走私向量）
}

TEST_CASE(DuplicateContentLengthSameValue) {
    HttpContext ctx = parseOnce("POST /a HTTP/1.1\r\nHost: x\r\n"
                                "Content-Length: 5\r\nContent-Length: 5\r\n\r\nhello");
    CHECK_EQ(ctx.error(), HttpContext::kNoError);               // 同值重复可容忍
    CHECK_EQ(ctx.request().body(), "hello");
}

TEST_CASE(NonNumericContentLength) {
    HttpContext ctx = parseOnce("POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 5abc\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kBadRequest);            // 前缀解析会把 "5abc" 当 5
    ctx = parseOnce("POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: -5\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kBadRequest);
    ctx = parseOnce("POST /a HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kBadRequest);            // 空值
}

TEST_CASE(LowercaseContentLength) {
    HttpContext ctx = parseOnce("POST /a HTTP/1.1\r\nHost: x\r\ncontent-length: 3\r\n\r\nabc");
    CHECK_EQ(ctx.error(), HttpContext::kNoError);               // 小写变体必须识别，
    CHECK_EQ(ctx.request().body(), "abc");                      // 否则 body 被当新请求行解析（错位）
}

// ---------- Transfer-Encoding（RFC 7230 §3.3.1） ----------

TEST_CASE(TransferEncodingRejected) {
    HttpContext ctx = parseOnce("POST /a HTTP/1.1\r\nHost: x\r\n"
                                "Transfer-Encoding: chunked\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kNotImplemented);        // TE → 501 显式拒绝（走私向量）
    ctx = parseOnce("POST /a HTTP/1.1\r\nHost: x\r\n"
                    "transfer-encoding: chunked\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kNotImplemented);        // 大小写变体同样拒绝
}

// ---------- Expect（RFC 7231 §5.1.1） ----------

TEST_CASE(Expect100Continue) {
    HttpContext ctx;
    Buffer buf;
    // 头还没收全（缺终止空行）：状态在 kExpectHeaders，不是回 100 的时机
    std::string part1 = "POST /a HTTP/1.1\r\nHost: x\r\n"
                        "Expect: 100-continue\r\nContent-Length: 5\r\n";
    buf.append(part1.data(), part1.size());
    ctx.parseRequest(&buf);
    CHECK(!ctx.shouldSendContinue());
    CHECK_EQ(ctx.state(), HttpContext::kExpectHeaders);
    // 头收全、进入 kExpectBody（确实有 body 要收）：应回 100
    buf.append("\r\n", 2);
    ctx.parseRequest(&buf);
    CHECK_EQ(ctx.error(), HttpContext::kNoError);
    CHECK_EQ(ctx.state(), HttpContext::kExpectBody);
    CHECK(ctx.shouldSendContinue());
    ctx.markContinueSent();
    CHECK(!ctx.shouldSendContinue());                           // 每请求只回一次
    // 补 body 后解析完成
    buf.append("hello", 5);
    CHECK(ctx.parseRequest(&buf));
    CHECK_EQ(ctx.state(), HttpContext::KGotCompleteRequest);
}

TEST_CASE(UnsupportedExpectation) {
    HttpContext ctx = parseOnce("POST /a HTTP/1.1\r\nHost: x\r\n"
                                "Expect: foobar\r\nContent-Length: 5\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kExpectationFailed);     // 无法满足的 Expect → 417
}

TEST_CASE(HugeBodyRejectedBeforeBodyArrives) {
    // 声明体长 17MB+（超过 kMaxBodyBytes）：头部解析完立即 413，不等 body 字节——
    // 不提前拦截会无界吃内存（Content-Length 可声明任意大小）
    HttpContext ctx = parseOnce("POST /a HTTP/1.1\r\nHost: x\r\n"
                                "Content-Length: 17825793\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kHeaderTooLarge);
    // 与 Expect: 100-continue 的交互：必须先回 413，绝不能先发 100 Continue 再反悔
    ctx = parseOnce("POST /a HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n"
                    "Content-Length: 17825793\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kHeaderTooLarge);
    CHECK(!ctx.shouldSendContinue());
}

// ---------- 头部限长（防 DoS） ----------

TEST_CASE(OverlongRequestLine) {
    HttpContext ctx = parseOnce("GET /" + std::string(9000, 'a') + " HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kHeaderTooLarge);        // 单行 > 8KB → 413
}

TEST_CASE(OverlongHeaderLine) {
    HttpContext ctx = parseOnce("GET /a HTTP/1.1\r\nHost: x\r\n"
                                "X-Big: " + std::string(9000, 'b') + "\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kHeaderTooLarge);
}

TEST_CASE(HeaderTotalBytesLimit) {
    // 单行都没超限，但累计 > 64KB → 413（无界头部会无限吃内存）
    std::string raw = "GET /a HTTP/1.1\r\nHost: x\r\n";
    for (int i = 0; i < 10; ++i) {
        raw += "X-H" + std::to_string(i) + ": " + std::string(7000, 'h') + "\r\n";
    }
    raw += "\r\n";
    HttpContext ctx = parseOnce(raw);
    CHECK_EQ(ctx.error(), HttpContext::kHeaderTooLarge);
}

// ---------- URL 解码（RFC 3986） ----------

TEST_CASE(UrlDecodePath) {
    HttpContext ctx = parseOnce("GET /a%20b%20c HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ctx.request().path(), "/a b c");
}

TEST_CASE(PlusStaysLiteralInPath) {
    // path 中 + 是合法 pchar（sub-delims），必须保持字面量（RFC 3986 §2.3）
    HttpContext ctx = parseOnce("GET /a+b HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ctx.request().path(), "/a+b");
}

TEST_CASE(PlusBecomesSpaceInQuery) {
    // query 才做 application/x-www-form-urlencoded 的 + → 空格（表单惯例）
    HttpContext ctx = parseOnce("GET /s?q=a+b HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ctx.request().path(), "/s");
    CHECK_EQ(ctx.request().query(), "q=a b");
}

TEST_CASE(PercentEncodedTraversalStaysLiteral) {
    // %2e%2e 解码成 ".." 是字面路径段，不做二次处理——
    // 穿越防护在 StaticFileHandler 的 realpath + isWithinRoot（解码后）
    HttpContext ctx = parseOnce("GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ctx.request().path(), "/../../etc/passwd");
}

TEST_CASE(InvalidPercentEncodingKept) {
    // 非法编码（%ZZ / 结尾悬空 %）必须原样保留，不能输出垃圾字节
    HttpContext ctx = parseOnce("GET /%ZZ%2G% HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ctx.request().path(), "/%ZZ%2G%");
}

// ---------- 头语法 ----------

TEST_CASE(HeaderKeyTrailingWhitespaceTrimmed) {
    // RFC 7230 §3.2.4："Connection : close" 的 key 是 "Connection "
    HttpContext ctx = parseOnce("GET /a HTTP/1.1\r\nHost: x\r\nConnection : close\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kNoError);
    CHECK_EQ(ctx.request().getHeader("Connection"), "close");
}

TEST_CASE(MalformedHeaderNoColon) {
    HttpContext ctx = parseOnce("GET /a HTTP/1.1\r\nHost: x\r\nBadHeaderLine\r\n\r\n");
    CHECK_EQ(ctx.error(), HttpContext::kBadRequest);
}

// ---------- 复用 ----------

TEST_CASE(ResetReuseBetweenRequests) {
    HttpContext ctx = parseOnce("GET /first HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK_EQ(ctx.state(), HttpContext::KGotCompleteRequest);
    CHECK_EQ(ctx.request().path(), "/first");

    ctx.reset();                                                // 请求处理完复位
    CHECK_EQ(ctx.request().path(), std::string());              // 旧请求数据已清空
    CHECK_EQ(ctx.state(), HttpContext::kExpectRequestLine);
    CHECK_EQ(ctx.error(), HttpContext::kNoError);

    Buffer buf;
    std::string second = "GET /second HTTP/1.1\r\nHost: b\r\nContent-Length: 2\r\n\r\nhi";
    buf.append(second.data(), second.size());
    CHECK(ctx.parseRequest(&buf));
    CHECK_EQ(ctx.request().path(), "/second");
    CHECK_EQ(ctx.request().getHeader("Host"), "b");
    CHECK_EQ(ctx.request().body(), "hi");
}

// ---------- Date 头（每秒缓存） ----------

// httpDateNow：返回 RFC 1123 IMF-fixdate 格式（"Mon, 02 Jan 2006 15:04:05 GMT"），
// 缓存不破坏正确性——值等于当前秒的 httpDate 格式化结果（允许秒边界 +1 的滚动）
TEST_CASE(DateHeader_CachedFormat) {
    time_t t = time(nullptr);
    const std::string& d = httpDateNow();
    CHECK_EQ(d.size(), std::string("Mon, 02 Jan 2006 15:04:05 GMT").size());
    CHECK_EQ(d.substr(d.size() - 3), std::string("GMT"));  // 结尾 GMT
    CHECK_EQ(d.substr(d.size() - 4), std::string(" GMT")); // GMT 前有空格（%H:%M:%S GMT）
    CHECK_EQ(d.substr(3, 1), std::string(","));            // "Fri," 星期缩写后的逗号
    CHECK(d[d.size() - 7] == ':');                         // 秒字段 ":SS GMT" 前的冒号
    // 与同秒内（或刚滚动到的下一秒）的 httpDate 逐字符一致
    CHECK(d == httpDate(t) || d == httpDate(t + 1));
    // 同一秒内两次调用复用同一缓存对象（地址稳定）
    CHECK_EQ(&d, &httpDateNow());
}
