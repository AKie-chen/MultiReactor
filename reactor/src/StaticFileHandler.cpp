#include "StaticFileHandler.h"
#include "Log.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
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
    std::string resolvedStr(resolved);
    return resolvedStr.rfind(rootDirAbs_, 0) == 0;
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

    // 3. 目录 → 尝试 index.html
    if (S_ISDIR(st.st_mode)) { // 判断是否是目录
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

        // 4. 设置文件作为响应体
        resp->setFileBody(indexResolved, st.st_size);
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->addHeader("Content-Type", getMimeType(indexResolved)); // 用实际文件(index.html)判定 MIME，目录路径无扩展名
        resp->addHeader("Content-Length", std::to_string(st.st_size));

        return true;
    }

    // 4. 检查文件大小
    if (st.st_size > maxFileSizeBytes_) {
        *resp = HttpResponse::makeError(HttpResponse::k413PayloadTooLarge, "Payload Too Large");
        return true;
    }

    // 5. 读取并返回（空文件 = 200 OK + 空 body）
    resp->setFileBody(resolved, st.st_size);
    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->addHeader("Content-Type", getMimeType(req.path()));
    resp->addHeader("Content-Length", std::to_string(st.st_size));
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