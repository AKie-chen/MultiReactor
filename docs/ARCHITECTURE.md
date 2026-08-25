# MultiReactor — 架构详解与设计决策

基于 Linux epoll ET 模式从零构建的 C++17 高性能 HTTP 服务器（零第三方依赖）。
本文档记录 17 步迭代（功能 + 稳定性/性能修复），每个优化对应独立的功能增量，
所有关键设计决策的完整论证见 [docs/DESIGN.md](DESIGN.md)。

## 架构

```
main
├── Config (CLI + key=value 配置文件, 两遍扫描 CLI 优先)
├── SignalHandler (eventfd + sigaction, SIGINT/SIGTERM 优雅关闭)
├── Logger (5 级过滤/时间戳/文件:行号, 行缓冲)
├── Metrics (6 个 atomic 计数器, lock-free)
├── Router + StaticFileHandler         # 业务层（worker 线程执行）
├── ThreadPool                         # 工作线程（one loop per thread, 在途有界 → 503 背压）
├── EventLoop (主线程)                 # accept + 信号 + 定时器
│   ├── TimerQueue (timerfd + 自实现最小堆, 1s 心跳扫描空闲超时)
│   ├── Acceptor (SO_REUSEADDR + EMFILE 排空)
│   └── EventLoopThread × N            # 子事件循环（连接 I/O）
└── TcpConnection                      # shared_ptr 管理, 内含 Channel + Buffer×2 + HttpContext
```

### 数据流 (一次 HTTP 请求)

```
epoll_wait → Channel::handleEvent (异常隔离: try/catch)
  → TcpConnection::handleRead (ET 循环读, readv + 64KB extrabuf)
    → HttpContext::parseRequest (状态机, 跨拆包累积, 错误分类)
      → ThreadPool::tryRun (RR 分发到 worker EventLoop, 在途满 → 503 背压)
        → Router::route (精确/参数化/通配符, 异常 → 500)
        → StaticFileHandler::handle (fallback, openat2 防 TOCTOU 穿越)
          → queueInLoop (切回 IO 线程, 异常隔离: try/catch)
            → TcpConnection::deliverResponse (按 seq 保序)
              → send/sendfile (直接写快路径, EAGAIN 才注册 EPOLLOUT)
```

## 特性

| 类别 | 内容 |
|------|------|
| I/O 模型 | epoll ET 边缘触发 + 非阻塞 I/O，readv + 64KB extrabuf 循环读，动态扩容，TCP_NODELAY，SO_KEEPALIVE |
| 缓冲区 | readv + prependable 三区模型，指数扩容/缩容 |
| 并发模型 | 主从 Reactor（主线程 accept + N 个 IO 线程 RR 分发）+ one-loop-per-thread 工作线程池（每 worker 独立 EventLoop，无共享队列，在途计数有界），eventfd 跨线程唤醒（wakeupPending_ 去重）；**请求级并行**：同连接多请求独立提交 worker，seq 有序响应队列按序重排发送（RFC 7230 §6.3.2） |
| 生命周期 | `shared_ptr` + `queueInLoop` 延迟析构，消除并发下 use-after-free（ASan 验证） |
| HTTP/1.1 | GET/POST/HEAD（HEAD 等价 GET 只发头，RFC 7231 §4.3.2），状态机解析，keep-alive / pipelining 保序；Host 头校验（RFC 7230 §5.4，缺失/重复冲突 → 400）、响应 Date 头（RFC 7231 §7.1.1.2，每秒缓存）、`Expect: 100-continue`、URL 解码按 RFC 3986（path 中 `+` 保持字面量，仅 query 做表单解码） |
| 协议安全 | 缺 Host / 重复 Host 冲突 → 400、CL+CL 冲突 → 400、TE → 501 显式拒绝（防走私）、无法满足的 Expect → 417、头部/body 限长 → 413（先于 100-continue）、505 版本不支持 |
| 路由 | 精确匹配 + 参数化（`/user/:id`）+ 通配符（`*`），URL 解码后匹配 |
| 静态文件 | MIME 映射，sendfile 零拷贝（>64KB）+ LRU 内容缓存（≤64KB），目录→index.html，304 协商缓存（Last-Modified/If-Modified-Since）；**TOCTOU 加固**：openat2 + RESOLVE_NO_SYMLINKS 原子校验，文件 fd 一次打开直传发送层（消除二次 open），老内核回退 O_NOFOLLOW + fstat dev/ino 比对 |
| 背压 | 线程池在途满 → 503 **不关连接**（避免"拒绝→重连→更忙"风暴，实测 TIME_WAIT 3000+ 的旧版对比） |
| 定时器 | timerfd + CLOCK_MONOTONIC + 自实现最小堆（O(log n) cancel），空闲连接超时；**惰性重置**：1s 心跳扫描替代每请求 cancel/addTimer（timerfd_settime 每请求 1 次 → 启动时 1 次） |
| 优雅关闭 | 信号 → eventfd → 停 accept → 排空在途请求（照常响应 + 强制 Connection: close）→ 连接归零退出，10s 兜底 |
| 异常隔离 | EventLoop 事件回调 + worker 任务三层 try/catch：业务 handler 异常 → 500 响应；逃逸异常 → 记日志、在途计数不泄漏、线程不退出 |
| 可观测性 | 6 个 lock-free atomic 指标 + `/stats` JSON，结构化日志，CLI/配置文件双源配置 |
| syscall 削减 | eventfd 唤醒去重（`wakeupPending_` 原子）、send 先直接写 EAGAIN 才注册 EPOLLOUT（消除 epoll_ctl 乒乓）、timerfd 惰性重置。strace 实测 1000 keep-alive 请求：epoll_ctl 11 次、timerfd_settime 5 次（旧实现各 2000/1000 次），每请求 ≈4 次 syscall |

## 快速开始

### 环境要求

- Linux (kernel ≥ 2.6.27, 需要 `timerfd_create` / `eventfd`；openat2 需 kernel ≥ 5.6，老内核自动回退)
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
./build/multireactor -p 9090 -i 2 -w 8 -d ./public -t 30 --log-level DEBUG

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
curl http://localhost:8080/user/42       # 200 参数路由
curl http://localhost:8080/stats         # 指标 JSON
curl -X POST http://localhost:8080/      # 405 Method Not Allowed
curl http://localhost:8080/nonexist      # 404 Not Found
curl -H "Content-Length: 104857600" -X POST http://localhost:8080/  # 413 提前拦截

# 压力测试
ab -n 10000 -c 100 http://127.0.0.1:8080/
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/
```

## 性能

测试环境：VMware 虚拟机 4 vCPU @ 3.2GHz（宿主机 Ryzen 7 7735H），4 IO + 2 worker
（默认配置），Release，本机回环，wrk 4 线程 10s。完整数据与瓶颈分析见
[README 性能节](../README.md)（下方为结论摘要）。

| 场景 | 吞吐 (req/s) | P50 | P99 |
|------|--------------|-----|-----|
| 动态路由 /user/123（-c1000） | 140,445 | 6.09 ms | 15.15 ms |
| 静态小文件（LRU 命中，-c100） | 74,480 | 1.21 ms | 3.34 ms |
| 大文件 300KB（sendfile，-c100） | 19,096 | 3.72 ms | 9.94 ms |

**瓶颈分析**（2026-08 worker 池重构后的结论）：

- **worker 池重构：共享队列 → one-loop-per-thread，-c1000 动态路由 +39%**：
  ThreadPool 从"单共享队列 + mutex/condvar"改为"N 个 worker 各自运行独立
  EventLoop，RR 分发 + eventfd 唤醒"——提交路径只剩两个原子操作（fetch_add
  在途计数 + RR 取模），无共享队列、无锁竞争、无 notify_one 空唤醒。同配置
  （4 IO + 2 worker）对比：-c1000 101.2k → 140.4k（+39%），-c100 80.4k →
  129.8k（+62%），P50 8.66 → 6.09 ms；503 比例从 1.2% 降至 0.5%
- **线程数结论不变**：4 核上 4+4（98.3k）仍显著低于 4+2 的 140.4k——worker
  过多只是超订抢占；规则仍为 **IO ≈ 核数，worker 取 2~3**
- 吞吐平台 ≈14 万 req/s，由 2 个 worker 各自的 loop 消费速率决定（每 worker
  ~7 万 tasks/s），而非 CPU（4 vCPU 理论峰值 ≈ 19.7 万，达成率 71%）。
  c=2000/5000 的 503 即突发提交溢出 1024 在途上限的背压
- syscall 削减效果（历史记录）：削减后每请求 ≈4 次系统调用，c=100 动态路由
  48.5k→52.9k（+9%）、静态小文件 67.4k→80.4k（+19%）

> 注：更早的历史测量（共享队列线程池，峰值 ~5.7 万 req/s）已随重构删除，
> 保留在 git 历史中。

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

### 并发安全 + 性能修复 (Step 9–15)

| # | 轮次 | 关键修复 |
|---|------|----------|
| 9 | shared_ptr 重构 | TcpConnection: `enable_shared_from_this`, 回调全改 `shared_ptr`, handleClose 防重入 (`closed_` atomic), fd 归 Channel 统一关闭, destroy 延迟释放守卫防 epoll 悬垂指针, TcpServer::connections_ 加 mutex |
| 10 | 6 项 BugFix | **P0** resetTimer 过期时间 `now + timeout` (原忘加 now→立即到期→keep-alive 失效); TimerQueue `map→multimap` 防同微秒覆盖, cancel 已取出 timer 同步清 `id2exp_`; EventLoop 锁缩小到 swap + `callingPendingFunctors_`; snprintf n 值 clamp 防越界读; events_ 动态扩容; Buffer prepend O(1) prependable 区 |
| 11 | 6 项 BugFix | **P0** TimerQueue `addTimer`/`cancel` 加 `assert(isInLoopThread())`; EPOLLRDHUP/EPOLLERR→errorCallback→handleClose; Buffer::append 指数扩容 (cap×2); ThreadPool `running_` `bool→atomic<bool>`; Log stdout 去 flush (行缓冲 `\n` 自动刷) |
| 12 | 压测 + 文档 | 并发-吞吐量曲线, 多场景 wrk, 零错误压测 |
| 13 | 整理 + 修复 | HEAD 等价 GET (RFC 7231), 请求头大小限制 (→413 防 DoS), Connection: close 尊重 (RFC 7230), max_connections 配置键修复, 文档/CI/License 完善 |
| 14 | 全量回归 + 修复 | TimerQueue 重构为自实现最小堆 (`timerHeap_` + `id2index_`); Router 参数模式跳过精确/`pathToMethods_` 查找 (`/user/%3Aid` 解码命中 404 bug); Content-Length 大小写不敏感 + 全数字校验 + body 16MB 上限 (防请求走私/DoS); getHeader 大小写不敏感; 404/413 分支补齐; `-m` 短参数 |
| 15 | 排空修复 | SIGTERM 后排空期请求静默丢弃 (客户端挂起超时) → 改为请求照常处理 + 响应强制 `Connection: close`; 在途响应完整送达, 连接归零即退出 |

### 架构重构 + 加固 (Step 16–17)

| # | 优化 | 关键产出 |
|---|------|----------|
| 16 | worker 池重构 + 请求级并行 | ThreadPool 共享队列 → **one-loop-per-thread**（每 worker 独立 EventLoop，RR 分发 + eventfd 唤醒，提交路径只剩两个原子操作）；同连接流水线请求独立提交 worker、`deliverResponse` 按 seq 暂存/补发保序（RFC 7230 §6.3.2，修复响应错配 `[200,400]→[400,400]`）；`markForClose` 先于投递评估修复短连接不关闭。实测 -c1000 动态路由 101.2k → **140.4k**（+39%），503 比例 1.2% → 0.5%；openat2 + RESOLVE_NO_SYMLINKS 关闭静态文件 TOCTOU 窗口（老内核回退 O_NOFOLLOW + fstat dev/ino） |
| 17 | 异常隔离 + Date 头缓存 | EventLoop 事件回调/pending functor 与 worker 任务三层 try/catch：handler 异常转 500（客户端不挂起），逃逸异常记日志不杀线程、在途计数不泄漏（优雅关闭不挂 10s）；`httpDateNow()` 线程本地每秒缓存替代每响应一次 `gmtime_r + strftime`（RFC 7231 §7.1.1.2 日期粒度就是秒） |

## 项目结构

```
MultiReactor/
├── include/   # 19 个头文件
│   ├── Acceptor.h              # listenfd 封装, accept 循环 + EMFILE 排空
│   ├── Buffer.h                # 非连续缓冲区 (readv, prependable 三区模型)
│   ├── Channel.h               # fd + events + callbacks 抽象
│   ├── Config.h                # 配置 struct + CLI/文件解析器
│   ├── EventLoop.h             # epoll 事件循环 (异常隔离, wakeup 去重)
│   ├── EventLoopThread.h       # EventLoop + thread 绑定
│   ├── HttpContext.h           # HTTP 请求解析状态机 (含头部限长/错误分类)
│   ├── HttpRequest.h           # HTTP 请求数据结构
│   ├── HttpResponse.h          # HTTP 响应序列化 + makeError + Date 每秒缓存
│   ├── Log.h                   # 结构化日志系统
│   ├── Metrics.h               # 指标单例 (atomic 计数器)
│   ├── Router.h                # URL 路由表 (精确 + 参数化 + 通配符)
│   ├── SignalHandler.h         # POSIX 信号 → eventfd 集成
│   ├── StaticFileHandler.h     # 静态文件 + LRU 缓存 + openat2 TOCTOU 防护
│   ├── TcpConnection.h         # 连接生命周期 + seq 保序响应队列
│   ├── TcpServer.h             # 服务器入口 + 连接计数
│   ├── ThreadPool.h            # 工作线程池 (one loop per thread, 在途有界)
│   ├── Timer.h                 # 定时器对象
│   └── TimerQueue.h            # timerfd + 自实现最小堆
├── src/       # 对应实现 + main.cpp（路由注册/排空状态机）
├── test/      # 66 个单元测试（零依赖 TEST_CASE 框架，ctest 接入）
├── docs/
│   ├── ARCHITECTURE.md         # 本文档（架构 + 17 步迭代记录）
│   └── DESIGN.md               # 逐模块设计决策 + 方案对比 + 底层原理
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

### 为什么 openat2 而不是仅 realpath 校验？
realpath 只消除"已有"符号链接，校验与 open 之间的替换窗口是 TOCTOU；openat2 + RESOLVE_NO_SYMLINKS 在 open 时刻原子校验全路径分量，老内核回退 O_NOFOLLOW + fstat dev/ino 比对（残余窗口仅中间分量）。

### 为什么大文件用 sendfile、小文件用内存缓存？
静态文件分两条路径：`> 64KB` 走 `sendfile(2)` 零拷贝——文件数据在内核态从 page cache 直接 DMA 到网卡，不经过用户态缓冲；`≤ 64KB` 走 LRU 内存缓存——高频小文件请求避免重复 open/close/read 系统调用（实测 100 并发静态小文件 74.5k req/s）。headers 与文件体分开发送：`send(2)` 先发响应头，`sendfile(2)` 再发文件体，两者可分别阻塞等待 EPOLLOUT。细节见 `TcpConnection::handleWrite` 的 SendItem 队列。

### 为什么 Metrics 用 atomic 而不是加锁？
6 个计数器分布在多个线程中并发写入，`std::atomic<uint64_t>` 的 `fetch_add` 在 x86 上是单条 `LOCK INC` 指令，比 mutex 快一个数量级。读取 `/stats` 时也不需要等锁。

### 为什么 TcpConnection 用 shared_ptr 而不是 delete this？
裸指针 + `delete this` 模式在并发场景下必然 use-after-free：worker 线程持有裸指针时，IO 线程可能已关闭连接并 delete。改为 `enable_shared_from_this` + `shared_ptr` 后，worker/定时器/IO 回调各自持有引用计数，最后一个释放时自动析构。`destroy()` 中 `queueInLoop([guard=shared_from_this()]{})` 的延迟释放守卫确保 epoll for 循环中的悬垂 Channel 指针安全。

### 为什么做三层异常隔离？
单连接/单请求的异常不应杀死整个进程（`std::terminate` 前默认就是如此）。三层防线：
1. **业务层**（main.cpp worker 任务）：handler/静态文件异常 → 记日志 + 转 500 响应，客户端得到显式错误而非挂起等超时；
2. **任务层**（ThreadPool 包裹）：任何逃逸异常（含包装层自身 bug）都不杀死 worker 线程，且 `inFlight_` 无条件自减——漏减会让在途计数虚高，引发误判背压（多余 503）与优雅关闭永不收敛（挂满 10s）；
3. **事件层**（EventLoop）：IO 线程上所有回调（连接读写/定时器/信号）抛异常只影响该连接，后续就绪事件照常处理，`callingPendingFunctors_` 复位不被跳过。

### 为什么 Date 头做每秒缓存？
RFC 7231 §7.1.1.2 要求所有响应带 Date，且日期粒度是秒——同一秒内所有响应的 Date 头必须相同。全量响应序列化每请求取一次，14 万 req/s 下每请求一次 `gmtime_r + strftime`（两次系统调用 + locale 处理 ≈ 数百 ns）是纯浪费。`thread_local` 缓存上次的秒值与格式化结果：响应序列化只发生在 IO 线程（deliverResponse），各线程独立缓存，无锁无伪共享，秒边界首次访问才重算。

## 踩坑记录（调试故事）

详见 [README 踩坑记录](../README.md)——响应错配、短连接不关闭、优雅关闭挂起、
EMFILE 忙循环、ASan UAF 等 10 个真实调试故事都记录在那里，本文档的迭代表
（Step 9–17）从代码维度给出了对应修复。

## 已知局限

- HTTP 协议仅支持 GET/POST/HEAD，不支持 chunked transfer-encoding（TE → 501 显式拒绝）
- URL 解码仅支持 `%xx` 与 query 的 `+`→空格，非法编码原样保留，不支持 UTF-8 规范化
- 线程池在途满时返回 503，无更复杂的背压策略（如按优先级丢弃/退避）
- 无 SSL/TLS
- 无 HTTP/2、WebSocket
- 路由支持精确/参数化/通配符，但不支持正则
- 指标无延迟分位数（histogram）；日志直接写终端无异步落盘

## 参考资料

- [muduo — 陈硕的 C++ 网络库](https://github.com/chenshuo/muduo)
- [The C10K Problem](http://www.kegel.com/c10k.html)
- Linux man: `epoll(7)`, `timerfd_create(2)`, `eventfd(2)`, `readv(2)`, `openat2(2)`, `realpath(3)`, `sendfile(2)`
- RFC 7230（HTTP/1.1 消息语义与路由）、RFC 7231（方法/状态码）、RFC 3986（URI）
