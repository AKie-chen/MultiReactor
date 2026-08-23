#include "test_framework.h"
#include "StaticFileHandler.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

// 临时静态目录夹具：mkdtemp + 预置文件/符号链接，析构时整体清理。
// 每个 TEST_CASE 独立实例 → 用例间隔离、顺序无关
struct TempStaticDir {
    std::string dir;
    std::string root;  // 带尾斜杠，StaticFileHandler 用

    TempStaticDir() {
        char tmpl[] = "/tmp/mr_test_static_XXXXXX";
        CHECK(mkdtemp(tmpl) != nullptr);
        dir = tmpl;
        root = dir + "/";
        writeFile("index.html", "<h1>index</h1>");
        writeFile("small.txt", "hello world");
        writeFile("big.bin", std::string(70 * 1024, 'B'));    // 70KB > 64KB 缓存阈值 → sendfile fd 路径
        writeFile("huge.bin", std::string(90 * 1024, 'H'));   // 90KB > maxFileSize(80KB) → 413
        writeFile("empty.txt", "");
        ::mkdir((dir + "/subdir").c_str(), 0755);             // 无 index.html 的目录
        ::symlink("/etc/passwd", (dir + "/escape").c_str());        // 指向 root 之外
        ::symlink("small.txt", (dir + "/link_small.txt").c_str());  // root 之内的链接
    }

    ~TempStaticDir() {
        for (const char* n : {"index.html", "small.txt", "big.bin", "huge.bin",
                              "empty.txt", "escape", "link_small.txt"}) {
            ::unlink((dir + "/" + n).c_str());
        }
        ::rmdir((dir + "/subdir").c_str());
        ::rmdir(dir.c_str());
    }

    void writeFile(const std::string& name, const std::string& content) {
        int fd = ::open((root + name).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0);
        if (fd >= 0) {
            ::write(fd, content.data(), content.size());
            ::close(fd);
        }
    }
};

static HttpRequest makeReq(HttpRequest::Method m, const std::string& path) {
    HttpRequest req;
    req.setMethod(m);
    req.setPath(path);
    return req;
}

// ---------- 基本行为 ----------

TEST_CASE(IndexHtml) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    CHECK(h.handle(makeReq(HttpRequest::kGet, "/"), &resp));
    CHECK_EQ(resp.code(), HttpResponse::k200Ok);
    // 目录 index.html 走文件体（fd）路径而非内存 body
    CHECK(resp.isFileBody());
    CHECK_EQ(resp.fileBodySize(), off_t(14));
    std::string content(14, '\0');
    CHECK_EQ(::pread(resp.fileFd()->fd, &content[0], 14, 0), ssize_t(14));
    CHECK_EQ(content, "<h1>index</h1>");
    CHECK_EQ(resp.header_value("Content-Type"), "text/html; charset=utf-8");
}

TEST_CASE(SmallFile) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    CHECK(h.handle(makeReq(HttpRequest::kGet, "/small.txt"), &resp));
    CHECK_EQ(resp.code(), HttpResponse::k200Ok);
    CHECK_EQ(resp.body(), "hello world");
}

TEST_CASE(EmptyFile) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    CHECK(h.handle(makeReq(HttpRequest::kGet, "/empty.txt"), &resp));
    CHECK_EQ(resp.code(), HttpResponse::k200Ok);
    CHECK(resp.body().empty());
}

TEST_CASE(CacheInvalidationOnMtimeChange) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    CHECK(h.handle(makeReq(HttpRequest::kGet, "/small.txt"), &resp));
    CHECK_EQ(resp.body(), "hello world");
    // 改写内容并把 mtime 拨到未来 2s：缓存必须失效并读到新内容
    d.writeFile("small.txt", "changed content");
    struct timespec ts[2];
    ts[0].tv_nsec = UTIME_NOW;
    ts[1].tv_sec = time(nullptr) + 2;
    ts[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, (d.root + "small.txt").c_str(), ts, 0);
    CHECK(h.handle(makeReq(HttpRequest::kGet, "/small.txt"), &resp));
    CHECK_EQ(resp.code(), HttpResponse::k200Ok);
    CHECK_EQ(resp.body(), "changed content");
}

TEST_CASE(NotModified) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    CHECK(h.handle(makeReq(HttpRequest::kGet, "/small.txt"), &resp));
    std::string lm = resp.header_value("Last-Modified");
    CHECK(!lm.empty());
    // If-Modified-Since = Last-Modified → 304 且无 body
    HttpRequest req = makeReq(HttpRequest::kGet, "/small.txt");
    req.addHeader("If-Modified-Since", lm);
    HttpResponse resp2;
    CHECK(h.handle(req, &resp2));
    CHECK_EQ(resp2.code(), HttpResponse::k304NotModified);
}

TEST_CASE(BigFileServedViaFd) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    CHECK(h.handle(makeReq(HttpRequest::kGet, "/big.bin"), &resp));
    CHECK_EQ(resp.code(), HttpResponse::k200Ok);
    CHECK(resp.isFileBody());
    CHECK_EQ(resp.fileBodySize(), off_t(70 * 1024));
    // fd-first：内容直接取自打开时校验过的 fd（TcpConnection 将 sendfile 它，不再二次 open）
    CHECK(resp.fileFd() != nullptr);
    std::string content(70 * 1024, '\0');
    ssize_t n = ::pread(resp.fileFd()->fd, &content[0], content.size(), 0);
    CHECK_EQ(n, ssize_t(70 * 1024));
    for (char c : content) CHECK(c == 'B');
}

TEST_CASE(FileTooLarge) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    CHECK(h.handle(makeReq(HttpRequest::kGet, "/huge.bin"), &resp));  // 90KB > 80KB
    CHECK_EQ(resp.code(), HttpResponse::k413PayloadTooLarge);
}

TEST_CASE(MethodRestriction) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    CHECK(!h.handle(makeReq(HttpRequest::kPost, "/small.txt"), &resp));  // 仅 GET/HEAD
    CHECK(h.handle(makeReq(HttpRequest::kHead, "/small.txt"), &resp));   // HEAD 允许（body 由 TcpConnection 裁）
    CHECK_EQ(resp.code(), HttpResponse::k200Ok);
}

TEST_CASE(MissingFile) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    CHECK(!h.handle(makeReq(HttpRequest::kGet, "/missing.txt"), &resp));  // → 调用方 404
}

TEST_CASE(DirectoryWithoutIndex) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    CHECK(!h.handle(makeReq(HttpRequest::kGet, "/subdir"), &resp));
}

TEST_CASE(DisabledHandler) {
    StaticFileHandler h("/nonexistent_dir_xyz_12345", 1024);  // 目录不存在 → 服务关闭
    HttpResponse resp;
    CHECK(!h.handle(makeReq(HttpRequest::kGet, "/anything"), &resp));
}

// ---------- 路径穿越 ----------

TEST_CASE(DotDotTraversal) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    CHECK(h.handle(makeReq(HttpRequest::kGet, "/../etc/passwd"), &resp));
    CHECK_EQ(resp.code(), HttpResponse::k403Forbidden);
}

TEST_CASE(EncodedDotDotTraversal) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    // 管线等价：HttpContext 会把 %2e%2e 解码为 ".."（解码单测见
    // test_http::PercentEncodedTraversalStaysLiteral），handler 收到的是解码后的路径。
    // 编码绕过在解码这一步就失效（黑名单式防护可被编码绕过，这里是解码后校验）
    CHECK(h.handle(makeReq(HttpRequest::kGet, "/../../etc/passwd"), &resp));
    CHECK_EQ(resp.code(), HttpResponse::k403Forbidden);
}

// ---------- TOCTOU / 符号链接 ----------

TEST_CASE(SymlinkEscapeToOutsideRoot) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    // 静态符号链接在 realpath 阶段就被解析出 root → 403（绝不回 /etc/passwd 的内容）
    CHECK(h.handle(makeReq(HttpRequest::kGet, "/escape"), &resp));
    CHECK_EQ(resp.code(), HttpResponse::k403Forbidden);
    CHECK(resp.body().find("root:") == std::string::npos);  // 不含 passwd 内容
}

TEST_CASE(SymlinkInsideRootServed) {
    TempStaticDir d;
    StaticFileHandler h(d.root, 80 * 1024);
    HttpResponse resp;
    // 静态符号链接在 realpath 阶段就被解析为规范路径（link_small.txt → small.txt 在
    // root 内，isWithinRoot 通过）→ 正常服务。openat2 的 RESOLVE_NO_SYMLINKS 防的是
    // realpath 校验之后、open 之前的分量被替换成链接的竞态窗口（TOCTOU），
    // 而非静态已存在的链接——后者已被 realpath 规范化 + 前缀比对约束
    CHECK(h.handle(makeReq(HttpRequest::kGet, "/link_small.txt"), &resp));
    CHECK_EQ(resp.code(), HttpResponse::k200Ok);
    CHECK_EQ(resp.body(), "hello world");
}
