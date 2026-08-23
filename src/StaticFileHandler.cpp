#include "StaticFileHandler.h"
#include "Log.h"
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/openat2.h>  // struct open_how / RESOLVE_NO_SYMLINKS（内核 ≥5.6）

// 无竞态打开静态文件：关闭 realpath 校验与 open 之间的 TOCTOU 窗口
// （攻击者可把刚校验过的路径分量替换成符号链接/其他文件，诱导读取 root 之外内容）。
// primary：openat2 + RESOLVE_NO_SYMLINKS（Linux 5.6+）。resolved 是 realpath 输出
// （全规范路径），要求路径**所有分量（含最后一段）**都不是符号链接——校验后任一
// 分量被替换成链接 → ELOOP → 拒绝。校验与打开之间不存在任何可利用的竞态窗口
// 回退（老内核 ENOSYS）：open(O_NOFOLLOW) + fstat 与打开前 stat 比对 dev/ino——
// 最后一段被换成别的文件 → inode 不符 → 拒绝；最后一段被换成链接 → O_NOFOLLOW
// 直接失败。残余窗口：中间分量被换成链接（stat 与 open 一致地跟随，比对失效）——
// 仅回退路径存在，openat2 路径无此窗口
static FileFd openResolved(const char* resolved)
{
    struct stat st;
    if (stat(resolved, &st) != 0) return nullptr;   // 回退比对用的快照（先于任何 open）
#ifdef SYS_openat2
    struct open_how how = {};
    how.flags = O_RDONLY | O_CLOEXEC;
    how.resolve = RESOLVE_NO_SYMLINKS;
    int fd = static_cast<int>(syscall(SYS_openat2, AT_FDCWD, resolved, &how, sizeof(how)));
    if (fd >= 0) return makeFileFd(fd);
    if (errno != ENOSYS) return nullptr;   // ELOOP/EACCES/ENOENT：被替换或已不存在
#endif
    int fb = ::open(resolved, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fb < 0) return nullptr;            // ELOOP：最后一段已是符号链接
    struct stat st2;
    if (fstat(fb, &st2) != 0 || st2.st_dev != st.st_dev || st2.st_ino != st.st_ino) {
        ::close(fb);                       // 最后一段被换成别的文件：inode 不符
        return nullptr;
    }
    return makeFileFd(fb);
}

// 从已打开的 fd 读取完整内容（TOCTOU 修复后文件在打开时已校验，不再按路径二次打开）
static std::string readFd(int fd, size_t size)
{
    std::string content(size, '\0');
    size_t got = 0;
    while (got < size) {
        ssize_t n = ::read(fd, &content[got], size - got);
        if (n > 0) { got += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;   // EOF / 错误
    }
    content.resize(got);
    return content;
}

StaticFileHandler::StaticFileHandler(const std::string& rootDir, size_t maxFileSizeBytes)
    : rootDir_(rootDir), maxFileSizeBytes_(maxFileSizeBytes), enabled_(false) {
    if (rootDir_[rootDir_.length() - 1] != '/') {
        rootDir_ += '/';
    }
    if (realpath(rootDir_.c_str(), rootDirAbs_) == nullptr) {
        LOG_WARN << "StaticFileHandler: directory does not exist — " << rootDir_
                 << " (static file serving disabled)";
        rootDirAbs_[0] = '\0';
        return;
    }
    enabled_ = true;
    LOG_INFO << "StaticFileHandler rootDir: " << rootDirAbs_;
}

bool StaticFileHandler::isWithinRoot(const char* resolved) const {
    if(strcmp(resolved, rootDirAbs_) == 0) return true; // resolved == rootDirAbs_，允许访问根目录
    // 斜杠必须加在"被查找的根前缀"上：rootDirAbs_ 是 realpath 输出（无尾斜杠），
    // 直接前缀匹配会让 "/static" 匹配上 "/static-evil/..."（穿越）。
    // 加在 resolved 上没用——那改变不了"rootDirAbs_ 是 resolved 的前缀"这个判定。
    std::string rootPrefix(rootDirAbs_);
    rootPrefix += '/';
    return std::string(resolved).rfind(rootPrefix, 0) == 0;
}

// 反向解析 If-Modified-Since → time_t，失败返回 false
static bool parseHttpDate(const std::string& s, time_t* out) {
    struct tm tm;
    if (strptime(s.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm) == nullptr) {
        return false;
    }
    *out = timegm(&tm);
    return true;
}

// 返回 true 表示成功处理（文件不存在时返回 false，由调用方给 404）
bool StaticFileHandler::handle(const HttpRequest& req, HttpResponse* resp)
{
    if (!enabled_) return false;

    // 静态文件只允许 GET / HEAD
    if (req.method() != HttpRequest::kGet && req.method() != HttpRequest::kHead) {
        return false;
    }

    // ".." 路径段前置拒绝：文件系统中不存在名为 ".." 的文件（POSIX 规定它是父目录项），
    // 任何解码后的 ".." 段都是穿越尝试，统一 403。为何不靠 realpath 兜底：glibc 的
    // realpath 对含 ".." 的路径行为不一致（"dir//.." 直接 ENOENT 而非解析到上层），
    // 前置检查让穿越响应语义确定（403 而非 404）。realpath + isWithinRoot 仍是
    // 符号链接穿越的兜底防线
    {
        size_t start = 1;  // 跳过前导 '/'
        const std::string& p = req.path();
        while (start < p.size()) {
            size_t end = p.find('/', start);
            if (end != std::string::npos) {
                if (p.compare(start, end - start, "..") == 0) {  // 精确匹配 ".."（"..." 是合法文件名）
                    *resp = HttpResponse::makeError(HttpResponse::k403Forbidden, "Forbidden");
                    return true;
                }
                start = end + 1;
            } else {
                if (p.compare(start, p.size() - start, "..") == 0) {
                    *resp = HttpResponse::makeError(HttpResponse::k403Forbidden, "Forbidden");
                    return true;
                }
                break;
            }
        }
    }

    std::string filepath = rootDir_ + req.path();
    char resolved[PATH_MAX];

    // 1. 解析路径（realpath 的返回同时是 openat2 的入参：全规范路径，无符号链接）
    if (realpath(filepath.c_str(), resolved) == nullptr) {
        return false;
    }
    if (!isWithinRoot(resolved)) {
        *resp = HttpResponse::makeError(HttpResponse::k403Forbidden, "Forbidden");
        return true;
    }

    // 2. 无竞态打开（TOCTOU 窗口在 openResolved 内关闭）。
    //    此后文件的所有元数据（大小/mtime/类型）都取自 fstat(fd)——即"实际打开
    //    的那个文件"。路径 stat 与 open 之间即使被换过，校验也与 fd 一致，
    //    不存在"按 A 的元数据、发 B 的内容"的错位
    FileFd fd = openResolved(resolved);
    if (!fd) return false;

    struct stat st;
    if (fstat(fd->fd, &st) != 0) return false;

    // 3. 目录 → 尝试 index.html（目录 fd 立即释放）
    if (S_ISDIR(st.st_mode)) {
        fd.reset();
        std::string indexPath = filepath + "index.html";
        char indexResolved[PATH_MAX];
        if (realpath(indexPath.c_str(), indexResolved) == nullptr) return false;
        if (!isWithinRoot(indexResolved)) {
            *resp = HttpResponse::makeError(HttpResponse::k403Forbidden, "Forbidden");
            return true;
        }
        FileFd indexFd = openResolved(indexResolved);
        if (!indexFd) return false;
        struct stat ist;
        // 仅接受常规文件：index.html 若是目录/FIFO 等，sendfile 会失败或挂起
        if (fstat(indexFd->fd, &ist) != 0 || !S_ISREG(ist.st_mode)) return false;
        if (ist.st_size > maxFileSizeBytes_) {
            *resp = HttpResponse::makeError(HttpResponse::k413PayloadTooLarge, "Payload Too Large");
            return true;
        }

        resp->setFileFd(indexFd, ist.st_size);
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->addHeader("Content-Type", getMimeType(indexResolved)); // 用实际文件(index.html)判定 MIME，目录路径无扩展名
        resp->addHeader("Content-Length", std::to_string(ist.st_size));
        resp->addHeader("Last-Modified", httpDate(ist.st_mtime));
        resp->addHeader("Cache-Control", "public, max-age=60");

        return true;
    }
    // 设备/FIFO/socket 等特殊文件：拒绝（sendfile 对它们会失败或语义错误）
    if (!S_ISREG(st.st_mode)) return false;

    // 4. 协商缓存
    std::string ims = req.getHeader("If-Modified-Since");
    time_t imsTime;
    if(req.method() == HttpRequest::kGet && !ims.empty() && parseHttpDate(ims, &imsTime) && st.st_mtime <= imsTime) {
        resp->setStatusCode(HttpResponse::k304NotModified);       // 需要新枚举值
        resp->addHeader("Last-Modified", httpDate(st.st_mtime));
        resp->addHeader("Cache-Control", "public, max-age=60");
        return true;   // 无 body → toString() 自然无 Content-Length，符合 304 规范
    }

    // 5. 检查文件大小
    if (st.st_size > maxFileSizeBytes_) {
        *resp = HttpResponse::makeError(HttpResponse::k413PayloadTooLarge, "Payload Too Large");
        return true;
    }

    // 6. 小文件走缓存
    if(st.st_size <= kCacheThreshold) {
        // 先查缓存
        {
            std::lock_guard<std::mutex> lock(mtx_);  // 保护 cacheMap_ 和 lruList_
            auto it = cacheMap_.find(resolved);
            if (it != cacheMap_.end()) {
                if (it->second.mtime == st.st_mtime) {  // 缓存命中
                    resp->setBody(it->second.content);        // 直接给内存里的内容,不读磁盘
                    // 更新 LRU:erase 旧迭代器 + push_front + 更新 lruIt
                    lruList_.erase(it->second.lruList_);
                    lruList_.push_front(resolved);
                    it->second.lruList_ = lruList_.begin();
                    resp->setStatusCode(HttpResponse::k200Ok);
                    resp->addHeader("Content-Type", getMimeType(resolved));
                    resp->addHeader("Content-Length", std::to_string(it->second.content.size()));
                    resp->addHeader("Last-Modified", httpDate(st.st_mtime));
                    resp->addHeader("Cache-Control", "public, max-age=60");
                    return true;
                }
                // mtime 失效：彻底移除旧 entry（map + list 同步清），
                // 否则 list 残留旧节点，缓存满时会把刚更新的新 entry 错误淘汰
                lruList_.erase(it->second.lruList_);
                cacheMap_.erase(it);
            }
        }

        // 缓存不存在或失效 → 从已打开的 fd 读文件
        std::string content = readFd(fd->fd, static_cast<size_t>(st.st_size));
        if (content.empty() && st.st_size > 0) { // 文件存在但读失败
            LOG_WARN << "Failed to read file: " << resolved;
            *resp = HttpResponse::makeError(HttpResponse::k500InternalServerError, "Internal Server Error");
            return true;
        }

        // 加入缓存
        {
            std::lock_guard<std::mutex> lock(mtx_);
            // 缓存文件内容（只缓存小文件）
            if (cacheMap_.size() >= kMaxCacheEntries) { // 淘汰最久未使用的
                const std::string& oldestPath = lruList_.back();
                cacheMap_.erase(oldestPath);
                lruList_.pop_back();
            }
            lruList_.push_front(resolved);
            cacheMap_[resolved] = {content, st.st_mtime, lruList_.begin()};

            resp->setBody(content);
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->addHeader("Content-Type", getMimeType(resolved));
            resp->addHeader("Content-Length", std::to_string(content.size()));
            resp->addHeader("Last-Modified", httpDate(st.st_mtime));
            resp->addHeader("Cache-Control", "public, max-age=60");
            return true;
        }
    }

    // 7. 大文件：fd 直接交给响应，TcpConnection 用 sendfile(fd) 发送——
    //    不再按路径二次打开（那会重新打开一个可能已被替换的窗口，见 sendFile）
    resp->setFileFd(fd, st.st_size);
    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->addHeader("Content-Type", getMimeType(resolved));
    resp->addHeader("Content-Length", std::to_string(st.st_size));
    resp->addHeader("Last-Modified", httpDate(st.st_mtime));
    resp->addHeader("Cache-Control", "public, max-age=60");
    return true;
}

std::string StaticFileHandler::getMimeType(const std::string& path)  // 后缀 → Content-Type
{
    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos) return "application/octet-stream";

    std::string suffix = path.substr(dotPos + 1);
    if (suffix == "html") return "text/html; charset=utf-8";
    if (suffix == "js") return "application/javascript";
    if (suffix == "css") return "text/css";
    if (suffix == "jpg" || suffix == "jpeg") return "image/jpeg";
    if (suffix == "png") return "image/png";
    if (suffix == "gif") return "image/gif";
    if (suffix == "svg") return "image/svg+xml";
    if (suffix == "ico") return "image/x-icon";
    if (suffix == "txt") return "text/plain; charset=utf-8";
    return "application/octet-stream"; // 其他文件类型
}
