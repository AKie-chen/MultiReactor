#pragma once
#include "HttpRequest.h"
#include "HttpResponse.h"
#include <string>
#include <climits>

class StaticFileHandler {
public:
    explicit StaticFileHandler(const std::string& rootDir, size_t maxFileSizeBytes = 10 * 1024 * 1024);

    // 返回 true 表示成功处理（包括 404，文件不存在也是"处理完了"）
    bool handle(const HttpRequest& req, HttpResponse* resp);

private:
    // 检查 resolved 路径是否在 rootDirAbs_ 之内（防路径穿越）
    bool isWithinRoot(const char* resolved) const;

    static std::string getMimeType(const std::string& path);   // 后缀 → Content-Type
    std::string readFile(const std::string& filepath);  // 读文件内容

    std::string rootDir_;        // 如 "./static"
    char rootDirAbs_[PATH_MAX];  // 如 "/home/user/project/static"
    size_t maxFileSizeBytes_;    // 单文件大小上限，超过返回 413
    bool enabled_;               // 目录存在且可用时为 true
};
