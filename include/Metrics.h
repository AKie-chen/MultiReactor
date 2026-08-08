#pragma once
#include <atomic>
#include <string>

struct Metrics {
    std::atomic<uint64_t> totalRequests{0}; // 总请求数
    std::atomic<uint64_t> activeConnections{0}; // 活跃连接数
    std::atomic<uint64_t> errors4xx{0}; // 4xx 错误数
    std::atomic<uint64_t> errors5xx{0}; // 5xx 错误数
    std::atomic<uint64_t> bytesReceived{0}; // 接收的字节数
    std::atomic<uint64_t> bytesSent{0}; // 发送的字节数

    static Metrics& instance();  // 单例
    std::string toJson() const;  // 序列化为 JSON

private:
    Metrics() = default;
};
