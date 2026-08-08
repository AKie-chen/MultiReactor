# MultiReactor

[![CI](https://github.com/AKie-chen/MultiReactor/actions/workflows/ci.yml/badge.svg)](https://github.com/AKie-chen/MultiReactor/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)]()

基于 **epoll ET** 从零实现的多线程 **Reactor 模式** C++17 HTTP 服务器。

核心设计参考 muduo：主从 Reactor 线程模型、`eventfd` 跨线程唤醒、`timerfd` 定时器、非阻塞 I/O + 三区缓冲区。提供路由、静态文件服务、结构化日志、优雅关闭、指标监控等生产级基础能力，在 4 核机器上实测吞吐 **5 万 req/s**。

## 特性

| 类别 | 内容 |
|------|------|
| I/O 模型 | epoll ET 边缘触发, 非阻塞 I/O, TCP_NODELAY, SO_KEEPALIVE |
| 并发模型 | 主从 Reactor (主线程 accept + N 个 IO 子线程) + 工作线程池 |
| 缓冲区 | readv 批量读取, prependable 三区模型, 自动扩容 |
| HTTP/1.1 | GET/POST/HEAD, Content-Length body, keep-alive, 状态机解析 (跨 TCP 拆包) |
| 错误处理 | 400 / 403 / 404 / 405 / 413 / 500 / 505, 按错误分类 |
| 路由 | 精确匹配 (method + path) + 参数化路由 (`/user/:id`) + 通配符 |
| 静态文件 | MIME 映射, realpath 路径穿越防护, LRU 内容缓存, 304 协商缓存, 大文件流式发送 |
| 定时器 | timerfd + CLOCK_MONOTONIC, O(log n) 取消, 空闲连接超时 |
| 日志 | 5 级过滤, 时间戳 + 文件:行号 |
| 信号 | SIGINT/SIGTERM 优雅关闭: 停止 accept → 排空活跃连接 → 退出 |
| 配置 | 命令行 + key=value 配置文件, CLI 优先 |
| 指标 | 6 个 lock-free atomic 计数器, `/stats` JSON 端点 |
| 安全 | 请求头大小限制 (单行 8KB / 累计 64KB → 413), 连接数上限, 503 背压 |

## 快速开始

### 环境要求

- Linux kernel ≥ 2.6.27（需要 `epoll` / `timerfd_create` / `eventfd`）
- CMake ≥ 3.10
- GCC ≥ 8 或 Clang ≥ 7（C++17）
- pthread

### 构建

```bash
git clone https://github.com/AKie-chen/MultiReactor.git
cd MultiReactor
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 运行

```bash
# 默认配置: 端口 8080, 4 IO 线程, 4 工作线程, 超时 10s, 静态目录 ./static
./build/multireactor

# 命令行参数
./build/multireactor -p 9090 -i 2 -w 8 -d ./public -t 30 --log-level DEBUG

# 配置文件 + CLI 覆盖 (CLI 优先级更高)
./build/multireactor -c server.conf -p 9090

# 查看全部选项
./build/multireactor -h
```

### 命令行参数

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `-p, --port` | 监听端口 | 8080 |
| `-i, --io` | IO 线程数 (sub event loops) | 4 |
| `-w, --workers` | 工作线程数 (路由/静态文件) | 4 |
| `-d, --static-dir` | 静态文件根目录 | ./static |
| `-t, --timeout` | 空闲连接超时 (秒) | 10 |
| `-m, --max-file-size` | 单个静态文件大小上限 (MB) | 10 |
| `-q, --max-queue-size` | 线程池队列上限 | 1024 |
| `--log-level` | TRACE/DEBUG/INFO/WARN/ERROR | INFO |
| `-c, --config` | 配置文件路径 | - |
| `-h, --help` | 帮助 | - |

### 配置文件格式

```ini
# server.conf
port = 8080
io_threads = 4
worker_threads = 4
static_dir = ./static
timeout = 30
log-level = INFO
max_file_size = 10
max_queue_size = 1024
max_connections = 10000
```

> `max_connections`（连接数上限）仅在配置文件中可设置，CLI 不支持。

## 测试

```bash
# 基础路由
curl http://localhost:8080/             # 200 Hello, World!
curl http://localhost:8080/user/42      # 200 user id: 42
curl http://localhost:8080/stats        # 200 指标 JSON

# 静态文件
curl http://localhost:8080/index.html   # 200 (目录自动找 index.html)
curl -I http://localhost:8080/a.txt     # HEAD: 200, 无 body

# 错误处理
curl http://localhost:8080/nonexist     # 404
curl -X POST http://localhost:8080/     # 405

# 压力测试
wrk -t4 -c100 -d10s http://127.0.0.1:8080/
```

本项目自带 GitHub Actions CI（`.github/workflows/ci.yml`），在 gcc/clang × Release/Debug 四组矩阵上构建并跑冒烟测试。

## 性能

测试环境: 4 核 Linux (Anolis OS 12, GCC 12.3), Release 编译 (`-O2`), 单机回环, wrk 默认配置。

| 并发连接 | 吞吐量 (req/s) | P50 | Max |
|----------|----------------|-----|-----|
| 100 | **49,587** | 1.98 ms | 26.17 ms |
| 500 | **49,006** | 10.05 ms | 27.07 ms |
| 1000 | **46,588** | 20.98 ms | 63.76 ms |
| 1500 | **29,208** | 50.68 ms | 111.82 ms |
| 2000 | **24,972** | 78.42 ms | 136.62 ms |

| 场景 | 配置 | 吞吐量 (req/s) | P50 |
|------|------|----------------|-----|
| 动态路由 (Hello World) | 100 conn × 10s | 49,587 | 1.98 ms |
| 静态文件 (缓存命中) | 100 conn × 10s | **61,839** | 1.64 ms |
| 高并发 | 2000 conn × 10s | 24,972 | 78.42 ms |

累计测试 **250 万+ 请求零错误**；ASan/UBSan 下混合流量（并发 + 错误请求 + 静态文件）零内存错误，SIGINT 优雅关闭排空活跃连接后干净退出。

> 延迟随并发线性增长，符合线程池排队效应的 Reactor 模型预期；4 核下饱和吞吐约 5 万 req/s，瓶颈在 CPU 而非架构。

## 项目结构

```
MultiReactor/
├── include/                  # 头文件
│   ├── EventLoop.h           # epoll 事件循环 (主线程 + 子线程)
│   ├── Channel.h             # fd + events + 回调 抽象
│   ├── Acceptor.h            # listenfd 封装, accept 循环
│   ├── TcpServer.h           # 服务器入口, 连接数管理
│   ├── TcpConnection.h       # 连接生命周期 (shared_ptr 管理)
│   ├── HttpContext.h         # HTTP 请求解析状态机 (限长)
│   ├── HttpRequest.h         # 请求数据结构
│   ├── HttpResponse.h        # 响应序列化 + 错误工厂
│   ├── Router.h              # 精确 + 参数化 + 通配符路由
│   ├── StaticFileHandler.h   # 静态文件 + LRU 缓存 + 防穿越
│   ├── TimerQueue.h          # timerfd 定时器队列
│   ├── ThreadPool.h          # 工作线程池 (有界队列)
│   ├── Buffer.h              # 非连续缓冲区
│   ├── Metrics.h             # lock-free 指标计数器
│   ├── SignalHandler.h       # 信号 → eventfd 集成
│   ├── Config.h              # 配置结构 + CLI/文件解析
│   └── Log.h                 # 结构化日志
├── src/                      # 实现
├── docs/
│   └── ARCHITECTURE.md       # 架构详解 + 关键设计决策
├── .github/workflows/ci.yml  # CI
├── CMakeLists.txt
└── LICENSE
```

## 架构概览

```
main
├── Config              命令行 + 配置文件
├── SignalHandler       SIGINT/SIGTERM → eventfd → 优雅关闭
├── Logger              5 级日志
├── Metrics             6 个 atomic 计数器
├── Router              路由表
├── StaticFileHandler   静态文件 + 缓存
├── ThreadPool          工作线程 (路由/静态文件处理)
├── EventLoop (主)      accept + 信号 + 定时器
│   ├── TimerQueue      timerfd, 连接超时
│   ├── Acceptor        监听 socket
│   └── EventLoopThread × N  子事件循环 (连接 I/O)
└── TcpConnection       HttpContext + Buffer + Channel + timer
```

一次 HTTP 请求的数据流: `epoll_wait → Channel::handleEvent → TcpConnection::handleRead → Buffer::readFd (ET 循环读) → HttpContext::parseRequest (状态机) → ThreadPool::tryRun (路由 + 静态文件) → EventLoop::queueInLoop (切回 IO 线程) → TcpConnection::send`。详细设计决策见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 已知局限

- HTTP 仅支持 GET/POST/HEAD，无 chunked transfer-encoding、URL 半角解码仅限 `%xx`、无 pipeline
- 线程池队列满时返回 503，无复杂背压策略
- 无 SSL/TLS、无 HTTP/2、无 WebSocket
- 静态文件使用 read + write 流式发送，未用 sendfile 零拷贝
- 路由不支持正则，仅精确匹配 + `:param` + `*` 通配
- 指标无延迟分位数 histogram

## License

[MIT](LICENSE)

## 参考

- [muduo — 陈硕的 C++ 网络库](https://github.com/chenshuo/muduo)
- [The C10K Problem](http://www.kegel.com/c10k.html)
- Linux man: `epoll(7)`, `timerfd_create(2)`, `eventfd(2)`, `readv(2)`, `realpath(3)`
