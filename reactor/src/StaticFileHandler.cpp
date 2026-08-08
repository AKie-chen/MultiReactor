#include "StaticFileHandler.h"
#include "Log.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

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

// 获取time_t对应的HTTP日期格式字符串
static std::string httpDate(time_t t) {
    char buf[128];
    struct tm tm;
    if(gmtime_r(&t, &tm) == nullptr) {
        return "";
    }
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return std::string(buf);
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

    std::string filepath = rootDir_ + req.path();
    char resolved[PATH_MAX];

    // 1. 解析路径
    if (realpath(filepath.c_str(), resolved) == nullptr) {
        return false;
    }
    if (!isWithinRoot(resolved)) {
        *resp = HttpResponse::makeError(HttpResponse::k403Forbidden, "Forbidden");
        return true;
    }

    // 2. stat 获取文件信息
    struct stat st;
    if (stat(resolved, &st) != 0) return false;

    // 3. 如果是目录 → 尝试 index.html
    if (S_ISDIR(st.st_mode)) { 
        // 是目录则查找默认索引文件
        std::string indexPath = filepath + "index.html";
        char indexResolved[PATH_MAX];
        if (realpath(indexPath.c_str(), indexResolved) == nullptr) return false;
        if (!isWithinRoot(indexResolved)) {
            *resp = HttpResponse::makeError(HttpResponse::k403Forbidden, "Forbidden");
            return true;
        }

        if (stat(indexResolved, &st) != 0) return false; // 判断 index.html 是否存在
        if (st.st_size > maxFileSizeBytes_) {
            *resp = HttpResponse::makeError(HttpResponse::k413PayloadTooLarge, "Payload Too Large");
            return true;
        }

        resp->setFileBody(indexResolved, st.st_size);
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->addHeader("Content-Type", getMimeType(indexResolved)); // 用实际文件(index.html)判定 MIME，目录路径无扩展名
        resp->addHeader("Content-Length", std::to_string(st.st_size));
        resp->addHeader("Last-Modified", httpDate(st.st_mtime));
        resp->addHeader("Cache-Control", "public, max-age=60");

        return true;
    }

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

        // 缓存不存在或失效 → 读文件
        std::string content = readFile(resolved);
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

    // 大文件
    // 7. 读取并返回（空文件 = 200 OK + 空 body）
    resp->setFileBody(resolved, st.st_size);
    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->addHeader("Content-Type", getMimeType(resolved));
    resp->addHeader("Content-Length", std::to_string(st.st_size));
    resp->addHeader("Last-Modified", httpDate(st.st_mtime));
    resp->addHeader("Cache-Control", "public, max-age=60");
    resp->addHeader("Accept-Ranges", "bytes"); // 支持断点续传
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

std::string StaticFileHandler::readFile(const std::string& filepath)  // 读文件内容
{
    std::ifstream file(filepath.c_str(), std::ios::binary);
    if (!file.is_open()){
        LOG_DEBUG << "Failed to open file: " << filepath;
        return "";
    }

    // 文件大小检查
    file.seekg(0, std::ios::end);
    long long size = file.tellg();

    // 如果文件大小超过限制，直接返回空字符串
    if(size > maxFileSizeBytes_){
        LOG_DEBUG << "File size exceeds limit: " << filepath;
        return "";
    }

    file.seekg(0, std::ios::beg); // 回到文件开头
    std::ostringstream buffer;
    if(file){
        buffer << file.rdbuf();
    }

    file.close();
    return buffer.str();
}