# MultiReactor — 架构详解与设计决策

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
├── ThreadPool (2 线程, 默认配置, 请求处理)
├── EventLoop (主线程, accept + 信号 + 定时器)
│   ├── TimerQueue (timerfd + 自实现最小堆, 连接超时管理)
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
            → ThreadPool::tryRun (工作线程, 有界队列满 → 503 背压)
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
| 静态文件 | MIME 映射, realpath 路径穿越防护, LRU 内容缓存 (≤64KB), sendfile 零拷贝 (>64KB), 304 协商缓存, 目录→index.html |
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
# 默认配置 (端口 8080, 4 IO 线程, 2 工作线程, 超时 10s)
./build/multireactor

# 命令行参数
./build/multireactor -p 9090 -i 4 -w 2 -d ./public -t 30 --log-level DEBUG

# 配置文件 + CLI 覆盖 (CLI 优先级高于文件)
./build/multireactor -c server.conf -p 9090

# 查看全部选项
./build/multireactor -h
```

### 配置文件格式

```ini
# server.conf
port = 8080
io_threads = 4
worker_threads = 2
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

测试环境: 4 IO + 2 worker 线程, Release 编译, 本机回环, wrk 4 线程 10s, 动态路由 `/user/123`。

> 注：本节数据为 2026-08 默认配置调整（4 IO + 4 worker → 4 IO + 2 worker）前的历史测量。
> 当前默认配置下动态路由峰值 ≈10.1 万 req/s（-c1000，wrk --latency），线程组合扫描与
> 瓶颈分析见 [README 性能节](../README.md)。

### 吞吐量 vs 并发度

| 并发连接 | 吞吐量 | P50 延迟 | Max 延迟 |
|----------|--------|----------|----------|
| 100 | 41,315 req/s | 2.39 ms | 12.90 ms |
| 500 | 52,742 req/s | 9.30 ms | 32.40 ms |
| 1000 | **57,522 req/s** | 16.80 ms | 59.18 ms |
| 2000 | 47,171 req/s | 41.41 ms | 125.64 ms |

### 多场景 Summary

| 场景 | 配置 | 吞吐量 | P50 |
|------|------|--------|-----|
| 动态路由 (Hello World) | 100 conn × 10s | 41,315 req/s | 2.39 ms |
| 静态文件 (LRU 缓存命中) | 100 conn × 10s | **62,188 req/s** | 1.62 ms |
| 大文件 300KB (sendfile 零拷贝) | 100 conn × 10s | 14,350 req/s | 6.56 ms |

> **稳定性**: 25 项协议测试 + 22 项功能回归全部通过；ASan/UBSan 下 300 并发混合畸形流量（并发 + 错误请求 + 静态文件）零内存错误；`--max-queue-size 1` 实测触发 503 背压；SIGTERM 压测中优雅关闭: 在途大文件响应完整送达（逐字节校验）、排空期请求正常响应并关闭连接、活跃连接归零后 0.1s 内退出。
>
> **瓶颈分析**: 吞吐峰值约 5.7 万 req/s（1000 并发），延迟随并发线性增长（线程池排队效应），符合 Reactor 模型预期。

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
| 14 | 全量回归 + 修复 | TimerQueue 重构为自实现最小堆 (`timerHeap_` + `id2index_`); Router 参数模式跳过精确/`pathToMethods_` 查找 (`/user/%3Aid` 解码命中 404 bug); Content-Length 大小写不敏感 + 全数字校验 + body 16MB 上限 (防请求走私/DoS); getHeader 大小写不敏感; 404/413 分支补齐; `-m` 短参数 |
| 15 | 排空修复 | SIGTERM 后排空期请求静默丢弃 (客户端挂起超时) → 改为请求照常处理 + 响应强制 `Connection: close`; 在途响应完整送达, 连接归零即退出 |

## 项目结构

```
MultiReactor/
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

### 为什么大文件用 sendfile、小文件用内存缓存？
静态文件分两条路径：`> 64KB` 走 `sendfile(2)` 零拷贝——文件数据在内核态从 page cache 直接 DMA 到网卡，不经过用户态缓冲（省一次 read + write 的用户态/内核态切换和拷贝）；`≤ 64KB` 走 LRU 内存缓存——高频小文件请求避免重复 open/close/read 系统调用（实测 100 并发静态文件 61.8k req/s 即受益于此）。headers 与文件体分开发送：`send(2)` 先发响应头，`sendfile(2)` 再发文件体，两者可分别阻塞等待 EPOLLOUT。细节见 `TcpConnection::handleWrite` 的 SendItem 队列。

### 为什么 Metrics 用 atomic 而不是加锁？
6 个计数器分布在 5 个线程中并发写入，`std::atomic<uint64_t>` 的 `fetch_add` 在 x86 上是单条 `LOCK INC` 指令，比 mutex 快一个数量级。读取 `/stats` 时也不需要等锁。

### 为什么 TcpConnection 用 shared_ptr 而不是 delete this？
裸指针 + `delete this` 模式在并发场景下必然 use-after-free：worker 线程持有裸指针时，IO 线程可能已关闭连接并 delete。改为 `enable_shared_from_this` + `shared_ptr` 后，worker/定时器/IO 回调各自持有引用计数，最后一个释放时自动析构。`destroy()` 中 `queueInLoop([guard=shared_from_this()]{})` 的延迟释放守卫确保 epoll for 循环中的悬垂 Channel 指针安全。

## 已知局限

- HTTP 协议仅支持 GET/POST/HEAD，不支持 chunked transfer-encoding
- URL 解码仅支持 `%xx` 与 `+`→空格，非法编码原样保留，不支持 UTF-8 规范化
- 线程池队列满时返回 503，无复杂背压策略
- 无 SSL/TLS
- 无 HTTP/2、WebSocket
- 路由支持精确/参数化/通配符，但不支持正则
- 指标无延迟分位数（histogram）

## 参考资料

- [muduo — 陈硕的 C++ 网络库](https://github.com/chenshuo/muduo)
- [The C10K Problem](http://www.kegel.com/c10k.html)
- Linux man: `epoll(7)`, `timerfd_create(2)`, `eventfd(2)`, `realpath(3)`
