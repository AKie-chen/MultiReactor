# MultiReactor

[![CI](https://github.com/AKie-chen/MultiReactor/actions/workflows/ci.yml/badge.svg)](https://github.com/AKie-chen/MultiReactor/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)]()

基于 **epoll ET** 从零实现的多线程 **Reactor 模式** C++17 HTTP 服务器。

核心设计参考 muduo：主从 Reactor 线程模型、`eventfd` 跨线程唤醒、`timerfd` 定时器、非阻塞 I/O + 三区缓冲区。提供路由、静态文件服务（含 sendfile 零拷贝）、结构化日志、优雅关闭、指标监控等生产级基础能力，实测吞吐 **6.7 万 req/s**（1000 并发，5 轮压测平均）。

## 特性

| 类别 | 内容 |
|------|------|
| I/O 模型 | epoll ET 边缘触发, 非阻塞 I/O, TCP_NODELAY, SO_KEEPALIVE, readv + 64KB extrabuf 循环读 |
| 并发模型 | 主从 Reactor（主线程 accept + N 个 IO 子线程, RR 分发）+ 有界队列工作线程池 |
| 缓冲区 | prependable 三区模型, 指数扩容 |
| HTTP/1.1 | GET/POST/HEAD, Content-Length body, keep-alive / pipelining（每连接串行处理, 响应严格保序）, 状态机解析（跨 TCP 拆包累积） |
| 解析健壮性 | 头名大小写不敏感（`Content-Length`/`connection:` 等变体均可识别）, 头名尾部空白 trim, Content-Length 全数字校验（拒绝 `5abc` 前缀解析）, body 声明超限立即 413 |
| 错误处理 | 400 / 403 / 404 / 405 / 413 / 500 / 501 / 503 / 505, 按错误分类; 413 触发: 请求行 >8KB / 头部累计 >64KB / body >16MB; 重复 Content-Length 值冲突 → 400（RFC 7230 §3.3.2）; Transfer-Encoding → 501（不支持, 不静默忽略） |
| 路由 | 精确匹配（method + path）+ 参数化（`/user/:id`）+ 通配符（`*`）; path 先 URL 解码（`%xx` + `+`→空格）后匹配, 连续斜杠折叠, 短 pattern 可匹配长 path |
| 静态文件 | MIME 映射, realpath + 前缀检查路径穿越防护（穿越 → 403, 符号链接逃逸也拦截）, LRU 内容缓存（≤64KB, 256 条, mtime 失效）, >64KB sendfile 零拷贝, 304 协商缓存（仅 GET; HEAD 不协商）, 目录自动 index.html, POST/PUT 静态资源 → 404 |
| 背压 | 线程池队列满 → 503 Service Unavailable; HTTP/1.1 下**不关连接**（拒绝后连接继续复用, 避免"拒绝→重连→更忙"风暴） |
| 定时器 | timerfd + CLOCK_MONOTONIC, 自实现最小堆, 空闲连接超时（默认 10s, 每次请求重置） |
| 日志 | 5 级过滤, 微秒时间戳 + 文件:行号 |
| 信号 | SIGINT/SIGTERM 优雅关闭: 停止 accept → 在途请求与排空期请求照常处理（响应强制 `Connection: close`）→ 活跃连接归零即退出, 10s deadline 兜底 |
| 配置 | 命令行 + key=value 配置文件（`log-level`/`log_level` 均可）, CLI 优先 |
| 指标 | 6 个 lock-free atomic 计数器（requests / active / err_4xx / err_5xx / bytes_recv / bytes_sent）, `/stats` JSON 端点 |
| 安全 | 头部与 body 大小限制（防 DoS）, 连接数上限（默认 10000）, 请求行非法字符校验 |

## 快速开始

### 环境要求

- Linux kernel ≥ 2.6.27（需要 `epoll` / `timerfd_create` / `eventfd` / `sendfile`）
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
| `-m, --max-file-size` | 单个静态文件大小上限 (MB), 超出 → 413 | 10 |
| `--max-queue-size` | 线程池队列上限 (满 → 503) | 1024 |
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

# 参数路由
curl http://localhost:8080/user/%3Aid   # 200 user id: :id（URL 解码后匹配参数模式）
curl http://localhost:8080/user//42     # 200 连续斜杠折叠
curl http://localhost:8080/user/a%20b   # 200 user id: a b

# 静态文件
curl http://localhost:8080/index.html   # 200 (目录自动找 index.html)
curl -I http://localhost:8080/big.bin   # HEAD: 200, 有 Content-Length 无 body

# 协商缓存
curl -I http://localhost:8080/test.txt  # 拿 Last-Modified
curl -H "If-Modified-Since: <Last-Modified>" -I http://localhost:8080/test.txt  # GET → 304

# 错误处理
curl http://localhost:8080/nonexist     # 404
curl -X POST http://localhost:8080/     # 405
curl -X POST http://localhost:8080/index.html  # 404（静态资源只接受 GET/HEAD）
curl -H "Content-Length: 104857600" -X POST http://localhost:8080/  # 413（body 超限, 不等数据到齐）

# 压力测试
wrk -t4 -c100 -d10s --latency http://127.0.0.1:8080/
```

本项目自带 GitHub Actions CI（`.github/workflows/ci.yml`），在 gcc/clang × Release/Debug 四组矩阵上构建并跑冒烟测试。

## 性能

测试环境: VMware 虚拟机, 4 vCPU @ 3.2GHz（宿主机 AMD Ryzen 7 7735H, 固定频率无睿频）, 4 IO + 4 worker 线程, Release 编译, 本机回环。**吞吐量为 5 轮 × 10s 压测取平均**（wrk `-t4`，各轮波动 < 5%），延迟为同负载档 10s 单轮实测。

| 并发连接 | 平均吞吐量 (req/s) | P50 | P99 | Max |
|----------|-------------------|-----|-----|-----|
| 100 | 48,490 | 1.91 ms | 5.28 ms | 10.22 ms |
| 500 | 62,108 | 7.43 ms | 17.07 ms | 26.46 ms |
| 1000 | **67,003** | 14.04 ms | 31.39 ms | 52.41 ms |
| 2000 | 62,040 | 29.48 ms | 69.63 ms | 107.26 ms |

| 场景 | 平均吞吐量 (req/s) | P50 | P99 |
|------|--------------------|-----|-----|
| 动态路由 /user/123（-c1000） | **67,003** | 14.04 ms | 31.39 ms |
| 静态小文件 test.txt（LRU 缓存命中） | 67,429 | 1.30 ms | 4.51 ms |
| 大文件 300KB big.bin（sendfile 零拷贝） | 15,820 | 5.46 ms | 14.30 ms |

> 5 轮原始数据 (req/s): -c100: 47700/47598/48880/48877/49392, -c500: 62414/62145/62974/60976/62030, -c1000: 65710/67880/66793/67590/67041, -c2000: 61588/59990/63940/64619/60063, test.txt: 65635/67540/67242/68393/68338, big.bin: 16144/15507/15802/15825/15820。

### 理论性能分析

每请求 CPU 成本（实测: 压测期间采样 `/proc/<pid>/stat`, -c1000 的 20s 窗口内 1,344,844 个请求共消耗 27.46 CPU 秒）：

- 每请求 **65,000 cycles** @ 3.2GHz（≈ 20.4 µs）
- 用户态 14,000 cycles（22%）: 状态机解析、路由匹配、响应序列化、`std::string` 堆分配
- 内核态 51,000 cycles（78%）: 每请求 **7 次系统调用**（readv / send / epoll_ctl×2 / timerfd_settime / eventfd×2）, 虚拟机内每次约 2.3 µs（VM-exit 虚拟化开销, 裸机约 0.1–0.3 µs）

**理论峰值**（CPU-bound 上限）: 4 vCPU × 3.2 GHz ÷ 65,000 cycles ≈ **19.7 万 req/s**。实测 6.7 万为其 34%。

差距归因:

1. **系统调用占绝对大头（78%）**: 每次请求 7 次 syscall, VMware 虚拟化使其成本放大一个量级。这是虚拟化环境的固有开销, 不是代码缺陷
2. **延迟受限而非 CPU 受限**: 实测 CPU 利用率仅 34%, 吞吐 ≈ 在途请求数 ÷ 端到端延迟（1000 连接 × ~14 ms ≈ 6.7 万, 与 Little's law 一致）; 每连接串行处理（响应严格保序的代价）限制单连接并发, 高吞吐靠多连接摊平
3. 剩余 CPU 空闲, 增加并发档位（-c2000）不再提升吞吐, 反而因调度开销略降

据此的优化方向: ① 减少系统调用 —— 当前 7 次/请求是设计天花板（timerfd 每请求重置、epoll_ctl enable/disable 乒乓、两次跨线程 eventfd 投递均可合并或消除）; ② io_uring 可将读/写/等待合入一次提交; ③ 裸机部署下 syscall 成本降约一个量级, 理论峰值可达数十万 req/s 量级。

验证矩阵: 25 项协议测试 + 22 项功能回归全部通过；ASan/UBSan 下 300 并发混合畸形流量零错误；`--max-queue-size 1` + 200 并发实测触发 503 背压（错误响应后连接保持, 无重连风暴）；SIGTERM 压测中优雅关闭: 在途大文件响应完整送达（逐字节校验）、排空期请求正常响应并关闭连接、活跃连接归零后 0.1s 内退出。

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
│   ├── TimerQueue.h          # timerfd + 最小堆定时器
│   ├── ThreadPool.h          # 工作线程池 (有界队列)
│   ├── Buffer.h              # 三区缓冲区
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
├── Logger             5 级日志
├── Metrics             6 个 atomic 计数器
├── Router              路由表 (精确 + 参数 + 通配符)
├── StaticFileHandler   静态文件 + LRU 缓存 + sendfile
├── ThreadPool          工作线程 (有界队列, 满 → 503 背压)
├── EventLoop (主)      accept + 信号 + 定时器
│   ├── TimerQueue      timerfd + 最小堆, 连接超时
│   ├── Acceptor        监听 socket
│   └── EventLoopThread × N  子事件循环 (连接 I/O)
└── TcpConnection       HttpContext + Buffer + Channel + timer
```

一次 HTTP 请求的数据流: `epoll_wait → Channel::handleEvent → TcpConnection::handleRead → Buffer::readFd (ET 循环读) → HttpContext::parseRequest (状态机) → ThreadPool::tryRun (路由 + 静态文件) → EventLoop::queueInLoop (切回 IO 线程) → TcpConnection::send`。详细设计决策见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 已知局限

- HTTP 仅支持 GET/POST/HEAD，无 chunked transfer-encoding（收到 Transfer-Encoding 头 → 501 拒绝）
- URL 解码仅支持 `%xx` 与 `+`→空格, 非法编码原样保留
- 无 SSL/TLS、无 HTTP/2、无 WebSocket
- 路由不支持正则，仅精确匹配 + `:param` + `*` 通配
- 线程池队列满时返回 503，无复杂背压策略
- 指标无延迟分位数 histogram

## License

[MIT](LICENSE)

## 参考

- [muduo — 陈硕的 C++ 网络库](https://github.com/chenshuo/muduo)
- [The C10K Problem](http://www.kegel.com/c10k.html)
- Linux man: `epoll(7)`, `timerfd_create(2)`, `eventfd(2)`, `readv(2)`, `realpath(3)`, `sendfile(2)`
