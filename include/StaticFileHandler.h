#pragma once
#include "HttpRequest.h"
#include "HttpResponse.h"
#include <string>
#include <climits>
#include <unordered_map>
#include <list>
#include <mutex>

class StaticFileHandler {
public:
    explicit StaticFileHandler(const std::string& rootDir, size_t maxFileSizeBytes = 10 * 1024 * 1024);

    // 处理静态文件请求，返回 true 表示成功处理（文件不存在时返回 false，由调用方给 404）
    bool handle(const HttpRequest& req, HttpResponse* resp);

private:
    struct CacheEntry {
        std::string content;  // 文件内容
        time_t mtime;   // 读入时的文件修改时间
        std::list<std::string>::iterator lruList_;  // 最近最少使用的文件路径列表
    };

    std::unordered_map<std::string, CacheEntry> cacheMap_;  // 缓存文件内容，key 为文件路径
    std::list<std::string> lruList_;  // 最近最少使用的文件路径列表，便于淘汰最久未使用的文件
    static constexpr size_t kCacheThreshold = 64 * 1024;  // 只缓存<=64kb
    static constexpr size_t kMaxCacheEntries = 256;  // 容量，超过则删除最久未使用的

    // 检查 resolved 路径是否在 rootDirAbs_ 之内（防路径穿越）
    bool isWithinRoot(const char* resolved) const;

    static std::string getMimeType(const std::string& path);   // 后缀 → Content-Type
    std::string readFile(const std::string& filepath);  // 读文件内容

    std::string rootDir_;        // 如 "./static"
    char rootDirAbs_[PATH_MAX];  // 如 "/home/user/project/static"
    size_t maxFileSizeBytes_;    // 单文件大小上限，超过返回 413
    bool enabled_;               // 目录存在且可用时为 true
    std::mutex mtx_;             // 保护 cacheMap_ 和 lruList_ 的互斥锁
};
