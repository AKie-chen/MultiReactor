#include "Metrics.h"

Metrics& Metrics::instance()  // 单例
{
    static Metrics s_instance;
    return s_instance;
}

std::string Metrics::toJson() const  // 序列化为 JSON
{
    std::string j;
    j += "{\"requests\":" + std::to_string(totalRequests.load())
      + ",\"active\":" + std::to_string(activeConnections.load())
      + ",\"err_4xx\":" + std::to_string(errors4xx.load())
      + ",\"err_5xx\":" + std::to_string(errors5xx.load())
      + ",\"bytes_recv\":" + std::to_string(bytesReceived.load())
      + ",\"bytes_sent\":" + std::to_string(bytesSent.load())
      + "}";
    return j;
}