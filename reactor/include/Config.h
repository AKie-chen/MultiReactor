#pragma once
#include <string>
#include <sys/socket.h>

struct ServerConfig{
    // 网络
    uint16_t port = 8080;
    std::string bindAddr = "0.0.0.0";
    int listenBacklog = SOMAXCONN; //listen积压队列大小
    bool tcpNoDelay = true;   // 是否关闭Nagle算法
    bool keepAlive = true; // 是否开启长连接
    size_t maxConnections = 10000; // 最大连接数
    int keepAliveIdleSec = 7200; // 长连接空闲时间(两小时)
    int maxQueueSize = 100; // 线程池队列最大长度

    // 线程
    size_t ioThreads = 4;
    size_t workerThreads = 4;

    // 静态文件
    std::string staticDir = "./static";
    size_t maxFileSizeMB = 10;

    // 超时
    int64_t connectionTimeoutSec = 10;

    // 日志
    std::string logLevel = "INFO";

};

class ConfigParser {
public:
    // 返回 false 表示需要打印 help 并退出
    bool parse(int argc, char* argv[], ServerConfig& cfg);
    
    // 从文件加载（key=value 格式）
    static bool loadFromFile(const std::string& filepath, ServerConfig& cfg);

private:
    static void printHelp(const char* program);
};