# Reactor — 架构详解与设计决策

基于 Linux epoll ET 模式从零构建的 C++17 高性能 HTTP 服务器。本文档记录 13 步迭代（功能 + 稳定性/性能修复），每个优化对应独立的功能增量。

## 架构

```
main
├── Config (CLI + 配置文件)
├── SignalHandler (eventfd + sigaction, 优雅关闭)
├── Logger (级别过滤/时间戳/文件行号)
├── Metrics (6 个 atomic 计数器, lock-free)
├── Router (精确路由: method + path → handler)
├── StaticFileHandler (磁盘文件服务 + 路径穿越防护)
├── ThreadPool (4 线程, CPU 密集任务)
├── EventLoop (主线程, accept + 信号 + 定时器)
│   ├── TimerQueue (timerfd + eventfd, 连接超时管理)
│   ├── TcpServer
│   │   └── Acceptor (SO_REUSEADDR + TCP_NODELAY)
│   └── EventLoopThread × N (sub loops, 连接 I/O)
└── TcpConnection (自包含: HttpContext + timerId + Channel + Buffer)
```

### 数据流 (一次 HTTP 请求)

```
epoll_wait → Channel::handleEvent
  → TcpConnection::handleRead
    → Buffer::readFd (readv, ET 模式)
      → Metrics::bytesReceived
        → HttpContext::parseRequest (状态机 + 错误分类)
          → Metrics::totalRequests++
            → ThreadPool::run (工作线程)
              → Router::route (精确匹配)
              → StaticFileHandler::handle (fallback, realpath 防穿越)
                → HttpResponse 构造 + 错误码统计
                  → EventLoop::queueInLoop (切回 I/O 线程)
                    → TcpConnection::send → ::send → Metrics::bytesSent
```

## 特性

| 类别 | 内容 |
|------|------|
| I/O 模型 | epoll ET, 非阻塞 I/O, TCP_NODELAY, SO_KEEPALIVE |
| 缓冲区 | readv, prependable 三区模型, 自动扩容/缩容 |
| HTTP | GET/POST/HEAD, Content-Length, 状态机, HEAD 等价 GET (RFC 7231), Connection 头 (RFC 7230) |
| 错误处理 | 400/403/404/405/413/500/505, 按错误分类 |
| 路由 | 精确匹配 + 参数化 (`/user/:id`) + 通配符 (`*`) |
| 静态文件 | MIME 映射, realpath 路径穿越防护, LRU 内容缓存, 304 协商缓存, 目录→index.html |
| 定时器 | timerfd + CLOCK_MONOTONIC, O(log n) cancel, 可配置超时 |
| 日志 | 结构化输出, 5 级过滤, 时间戳 + 文件:行号 |
| 信号 | SIGINT/SIGTERM 优雅关闭 (排空), eventfd 集成到 epoll |
| 配置 | CLI + key=value 配置文件, 两遍扫描 (CLI 优先) |
| 安全 | 请求头限长 (8KB/行, 64KB 累计 → 413), 连接数上限, 503 背压 |
| 指标 | 6 个 atomic 计数器, /stats JSON 端点, lock-free |
| 多线程 | 主从 Reactor + 线程池, eventfd 跨线程唤醒 |
| 协议 | HTTP/1.1, 支持 curl/ab/wrk |

## 快速开始

### 环境要求

- Linux (kernel ≥ 2.6.27, 需要 `timerfd_create` / `eventfd`)
- CMake ≥ 3.10
- GCC ≥ 8 或 Clang ≥ 7 (C++17)
- pthread

### 编译

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 运行

```bash
# 默认配置 (端口 8080, 4 IO 线程, 4 工作线程, 超时 10s)
./build/main

# 命令行参数
./build/main -p 9090 -i 2 -w 8 -d ./public -t 30 --log-level DEBUG

# 配置文件 + CLI 覆盖 (CLI 优先级高于文件)
./build/main -c server.conf -p 9090

# 查看全部选项
./build/main -h
```

### 配置文件格式

```ini
# server.conf
port = 8080
io_threads = 4
worker_threads = 4
static_dir = ./static
timeout = 10
log-level = INFO
max_file_size = 10
max_queue_size = 1024
max_connections = 10000   # 连接数上限, 仅配置文件支持
```

### 测试

```bash
# 功能验证
curl -v http://localhost:8080/           # 路由 → Hello, World!
curl http://localhost:8080/index.html    # 静态文件
curl http://localhost:8080/stats         # 指标 JSON
curl -X POST http://localhost:8080/      # 405 Method Not Allowed
curl http://localhost:8080/nonexist      # 404 Not Found

# 压力测试
ab -n 10000 -c 100 http://127.0.0.1:8080/
wrk -t4 -c100 -d30s http://127.0.0.1:8080/
```

## 性能

测试环境: 4 核 Linux (Anolis OS 12, GCC 12.3), 单机回环, Release 编译 (`-O2`), wrk 默认配置。

### 吞吐量 vs 并发度

| 并发连接 | 吞吐量 | P50 延迟 | Max 延迟 |
|----------|--------|----------|----------|
| 100 | **49,587 req/s** | 1.98 ms | 26.17 ms |
| 500 | **49,006 req/s** | 10.05 ms | 27.07 ms |
| 1000 | **46,588 req/s** | 20.98 ms | 63.76 ms |
| 1500 | **29,208 req/s** | 50.68 ms | 111.82 ms |
| 2000 | **24,972 req/s** | 78.42 ms | 136.62 ms |

### 多场景 Summary

| 场景 | 配置 | 吞吐量 | P50 |
|------|------|--------|-----|
| 动态路由 (Hello World) | 100 conn × 10s | **49,587 req/s** | 1.98 ms |
| 静态文件 (LRU 缓存命中) | 100 conn × 10s | **61,839 req/s** | 1.64 ms |
| 高并发 | 2000 conn × 10s | **24,972 req/s** | 78.42 ms |
| ab -k (keep-alive) | 2000 req × 50 conn | 9,789 req/s | 0 失败 |

> **稳定性**: 本次整理全量测试累计 **250 万+ 请求**零错误；ASan/UBSan 下混合流量（并发 + 错误请求 + 静态文件）零内存错误；SIGINT 优雅关闭排空活跃连接后干净退出。
>
> **瓶颈分析**: 4 核 CPU 极限下饱和吞吐约 5 万 req/s，延迟随并发线性增长（线程池排队效应），符合 Reactor 模型预期。

## 演进路线

### 功能迭代 (Step 1–8)

| # | 优化 | 关键产出 |
|---|------|----------|
| 1 | 结构化日志 | LogStream + Logger, 5 级过滤, 时间戳, `__FILE__`:`__LINE__` |
| 2 | 优雅关闭 | SignalHandler, eventfd + Channel 集成 POSIX 信号到 epoll |
| 3 | HTTP 错误处理 | 400/404/405/500/505, ParseError 分类, makeError 工厂 |
| 4 | 路由 + 静态文件 | Router (method+path 精确匹配), StaticFileHandler (realpath 防穿越, MIME) |
| 5 | 配置系统 | ServerConfig struct, CLI + 配置文件, 两遍扫描优先级 |
| 6 | TCP 优化 + 连接管理 | TCP_NODELAY, SO_KEEPALIVE, 可配置 backlog, 连接数上限 (atomic) |
| 7 | 指标监控 | Metrics 单例, 6 个 atomic 计数器, /stats JSON (lock-free) |
| 8 | 稳定性修复 | shutdown 完整关闭, pthread TPP 崩溃 workaround (SCHED_OTHER) |

### 并发安全 + 性能修复 (Step 9–12)

| # | 轮次 | 关键修复 |
|---|------|----------|
| 9 | shared_ptr 重构 | TcpConnection: `enable_shared_from_this`, 回调全改 `shared_ptr`, handleClose 防重入 (`closed_` atomic), fd 归 Channel 统一关闭, destroy 延迟释放守卫防 epoll 悬垂指针, TcpServer::connections_ 加 mutex |
| 10 | 6 项 BugFix | **P0** resetTimer 过期时间 `now + timeout` (原忘加 now→立即到期→keep-alive 失效); TimerQueue `map→multimap` 防同微秒覆盖, cancel 已取出 timer 同步清 `id2exp_`; EventLoop 锁缩小到 swap + `callingPendingFunctors_`; snprintf n 值 clamp 防越界读; events_ 动态扩容; Buffer prepend O(1) prependable 区 |
| 11 | 6 项 BugFix | **P0** TimerQueue `addTimer`/`cancel` 加 `assert(isInLoopThread())`; EPOLLRDHUP/EPOLLERR→errorCallback→handleClose; Buffer::append 指数扩容 (cap×2); ThreadPool `running_` `bool→atomic<bool>`; Log stdout 去 flush (行缓冲 `\n` 自动刷) |
| 12 | 压测 + 文档 | 并发-吞吐量曲线, 多场景 wrk, 零错误压测 |
| 13 | 整理 + 修复 | HEAD 等价 GET (RFC 7231), 请求头大小限制 (→413 防 DoS), Connection: close 尊重 (RFC 7230), max_connections 配置键修复, 文档/CI/License 完善 |

## 项目结构

```
LearningReactor/
├── include/
│   ├── Acceptor.h              # listenfd 封装, accept 循环
│   ├── Buffer.h                # 非连续缓冲区 (readv)
│   ├── Channel.h               # fd + events + callbacks 抽象
│   ├── Config.h                # 配置 struct + 解析器
│   ├── EventLoop.h             # epoll 事件循环
│   ├── EventLoopThread.h       # EventLoop + thread 绑定
│   ├── HttpContext.h           # HTTP 请求解析状态机 (含头部限长)
│   ├── HttpRequest.h           # HTTP 请求数据结构
│   ├── HttpResponse.h          # HTTP 响应序列化 + makeError
│   ├── Log.h                   # 结构化日志系统
│   ├── Metrics.h               # 指标单例 (atomic 计数器)
│   ├── Router.h                # URL 路由表 (精确 + 参数化 + 通配符)
│   ├── SignalHandler.h         # POSIX 信号 → eventfd 集成
│   ├── StaticFileHandler.h     # 静态文件 + LRU 缓存 + 路径穿越防护
│   ├── TcpConnection.h         # 连接生命周期管理
│   ├── TcpServer.h             # 服务器入口 + 连接计数
│   ├── ThreadPool.h            # 工作线程池 (有界队列)
│   ├── Timer.h                 # 定时器对象
│   └── TimerQueue.h            # timerfd 定时器队列
├── src/
│   ├── main.cpp                # 入口
│   └── *.cpp                   # 各模块实现
├── docs/
│   └── ARCHITECTURE.md         # 本文档
├── .github/workflows/ci.yml    # CI (gcc/clang × Release/Debug)
├── CMakeLists.txt
└── LICENSE
```

## 关键设计决策

### 为什么用 ET 而不是 LT？
ET 模式下每个事件只通知一次，必须循环读到 EAGAIN。好处是减少 epoll_wait 返回次数，缺点是实现更复杂。Buffer 的 readv + extrabuf 正是为此设计——一次读尽可能多的数据。

### 为什么延迟销毁 (queueInLoop)？
事件回调中不能 `delete this`——当前还在 `epoll_wait` 的 for 循环里，Channel 指针被 `events[i].data.ptr` 引用着。`queueInLoop` 把析构推迟到事件处理完毕后执行。

### 为什么每个 EventLoop 一个 TimerQueue？
定时器在哪个 loop 创建就在哪个 loop 触发。`addTimer` / `cancel` / `handleRead` 全程单线程，零锁竞争。

### 为什么 HttpContext 放在 TcpConnection 里？
消除 `std::map<TcpConnection*, HttpContext>` 的跨线程竞态。每个连接的 context 读写只在自己所属的 EventLoop 线程中发生。

### 为什么 realpath 而不是字符串禁止 `..`？
字符串黑名单可被 `//`、`%2e%2e`、符号链接绕过。`realpath` 解析规范路径后做前缀比较，同时验证文件存在性，一次系统调用解决两个问题。

### 为什么 Metrics 用 atomic 而不是加锁？
6 个计数器分布在 5 个线程中并发写入，`std::atomic<uint64_t>` 的 `fetch_add` 在 x86 上是单条 `LOCK INC` 指令，比 mutex 快一个数量级。读取 `/stats` 时也不需要等锁。

### 为什么 TcpConnection 用 shared_ptr 而不是 delete this？
裸指针 + `delete this` 模式在并发场景下必然 use-after-free：worker 线程持有裸指针时，IO 线程可能已关闭连接并 delete。改为 `enable_shared_from_this` + `shared_ptr` 后，worker/定时器/IO 回调各自持有引用计数，最后一个释放时自动析构。`destroy()` 中 `queueInLoop([guard=shared_from_this()]{})` 的延迟释放守卫确保 epoll for 循环中的悬垂 Channel 指针安全。

## 已知局限

- HTTP 协议仅支持 GET/POST/HEAD，不支持 chunked transfer-encoding、pipeline
- URL 解码仅支持 `%xx` 与 `+`，不支持 UTF-8 规范化
- 线程池队列满时返回 503，无复杂背压策略
- 无 SSL/TLS
- 无 sendfile/mmap 零拷贝（静态文件用 read + write）
- 无 HTTP/2、WebSocket
- 路由支持精确/参数化/通配符，但不支持正则
- 指标无延迟分位数（histogram）

## 参考资料

- [muduo — 陈硕的 C++ 网络库](https://github.com/chenshuo/muduo)
- [The C10K Problem](http://www.kegel.com/c10k.html)
- Linux man: `epoll(7)`, `timerfd_create(2)`, `eventfd(2)`, `realpath(3)`
