#pragma once
#include <sstream>
#include <string>
#include <functional>

// 日志级别
enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3, FATAL = 4 };

// LogStream — 收集一条日志的流式缓冲区，析构时输出整行
class LogStream {
public:
    LogStream(LogLevel level, const char* file, int line);
    ~LogStream();

    // 禁止拷贝
    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    // 允许移动
    LogStream(LogStream&& other) noexcept;
    LogStream& operator=(LogStream&& other) noexcept;

    // 重载 << ，支持各种类型的流式输出
    template<typename T>
    LogStream& operator<<(const T& val) {
        buf_ << val;
        return *this;
    }

private:
    LogLevel level_; // 日志级别
    const char* file_; // 文件名
    int line_; // 行号
    std::ostringstream buf_; // 日志内容缓冲区
    bool moved_ = false;  // 防止移动后析构时重复输出
};

// 日志输出回调 — 默认写 stderr，可替换为文件输出
class Logger {
public:
    using OutputCallback = std::function<void(LogLevel, const std::string&)>;

    // 设置全局最低日志级别（低于此级别的日志不会输出）
    static void setLevel(LogLevel level);
    static LogLevel level();
    static int parseLogLevel(const std::string& level); // 解析字符串为日志级别

    // 替换输出目标（默认 = 写 stderr）
    static void setOutput(OutputCallback cb);

    // 内部使用：输出一条日志行
    static void write(LogLevel level, const char* file, int line,
                      const std::string& message);

private:
    static LogLevel minLevel_; // 全局最低日志级别
    static OutputCallback output_; // 日志输出回调函数
    static const char* levelName(LogLevel level); // 获取日志级别名称
};

// 便捷宏 — 使用 __FILE__ 和 __LINE__ 自动捕获位置信息
#define LOG_DEBUG LogStream(LogLevel::DEBUG, __FILE__, __LINE__)
#define LOG_INFO  LogStream(LogLevel::INFO,  __FILE__, __LINE__)
#define LOG_WARN  LogStream(LogLevel::WARN,  __FILE__, __LINE__)
#define LOG_ERROR LogStream(LogLevel::ERROR, __FILE__, __LINE__)
#define LOG_FATAL LogStream(LogLevel::FATAL, __FILE__, __LINE__)
