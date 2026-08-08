#include "Log.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <mutex>
#include <cstdlib>   // abort()

// 静态成员初始化
LogLevel Logger::minLevel_ = LogLevel::DEBUG;
Logger::OutputCallback Logger::output_ = nullptr;

// ──── LogStream ────────────────────────────────────────────

LogStream::LogStream(LogLevel level, const char* file, int line)
    : level_(level), file_(file), line_(line) {}

LogStream::~LogStream() {
    if (!moved_) {
        Logger::write(level_, file_, line_, buf_.str());
        // FATAL 语义（muduo 风格）：日志输出后立即终止进程。
        // 带病继续跑只会掩盖错误——例如 bind 失败后静默启动、
        // 永不 accept（实测）。用 abort 而非 exit：以信号中止，
        // 不走析构不清栈，保证"启动失败"绝对可见
        if (level_ == LogLevel::FATAL) abort();
    }
}

LogStream::LogStream(LogStream&& other) noexcept
    : level_(other.level_),
      file_(other.file_),
      line_(other.line_),
      buf_(std::move(other.buf_)),
      moved_(false)
{
    other.moved_ = true;
}

LogStream& LogStream::operator=(LogStream&& other) noexcept {
    if (this != &other) {
        level_ = other.level_;
        file_  = other.file_;
        line_  = other.line_;
        buf_   = std::move(other.buf_);
        moved_ = false;
        other.moved_ = true;
    }
    return *this;
}

// ──── Logger ───────────────────────────────────────────────

void Logger::setLevel(LogLevel level) { minLevel_ = level; }

LogLevel Logger::level() { return minLevel_; }

int Logger::parseLogLevel(const std::string& level)
{
    if (level == "TRACE" || level == "trace") return 0;
    if (level == "DEBUG" || level == "debug") return 0;
    if (level == "INFO"  || level == "info")  return 1;
    if (level == "WARN"  || level == "warn")  return 2;
    if (level == "ERROR" || level == "error") return 3;
    if (level == "FATAL" || level == "fatal") return 4;
    return 1; // default INFO
}

void Logger::setOutput(OutputCallback cb) { output_ = std::move(cb); }

const char* Logger::levelName(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
    }
    return "?????";
}

void Logger::write(LogLevel level, const char* file, int line,
                   const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(minLevel_)) return;

    // 获取当前时间戳
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);

    // 格式化时间 — 线程安全版本
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);

    char timeStr[32];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &tm_buf);

    // 从完整路径中只取文件名（如 /Reactor/reactor/src/main.cpp → main.cpp）
    const char* filename = file;
    const char* p = file;
    while (*p) { if (*p == '/') filename = p + 1; ++p; }

    // 组装一行日志
    char lineBuf[1024];
    int n = snprintf(lineBuf, sizeof(lineBuf),
                     "[%s.%03lld] [%s] [%s:%d] %s\n",
                     timeStr, static_cast<long long>(ms.count()),
                     levelName(level), filename, line, message.c_str());

    // snprintf 返回实际需要的长度，可能超过 sizeof(lineBuf)；
    // 必须 clamp 到 buffer 大小，否则 lineBuf[n] 越界读
    if (n < 0) n = 0;
    if (n > static_cast<int>(sizeof(lineBuf))) n = sizeof(lineBuf);
    std::string logLine(lineBuf, n);

    // 输出
    static std::mutex outputMutex;
    std::lock_guard<std::mutex> lock(outputMutex);

    if (output_) {
        output_(level, logLine);
    } else {
        // 默认输出：INFO/DEBUG → stdout，WARN/ERROR/FATAL → stderr
        if (static_cast<int>(level) >= static_cast<int>(LogLevel::WARN)) {
            std::cerr << logLine << std::flush;  // 错误日志必须立即落盘
        } else {
            std::cout << logLine;  // stdout 行缓冲，'\n' 自动 flush，无需显式
        }
    }
}
