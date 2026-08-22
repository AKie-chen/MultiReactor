# MultiReactor

[![CI](https://github.com/AKie-chen/MultiReactor/actions/workflows/ci.yml/badge.svg)](https://github.com/AKie-chen/MultiReactor/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)]()

基于 **epoll ET** 从零实现的多线程 **Reactor 模式** C++17 HTTP 服务器，无任何第三方依赖。
核心设计参考 muduo（主从 Reactor、one loop per thread、eventfd 唤醒、timerfd 定时器），
实测吞吐 **6.7 万 req/s**（1000 并发，VM 环境）。

> 这是一个以学习为目的、以生产工程标准要求自己的项目：所有关键设计决策都经过
> 真实压测/ASan/UBSan 验证，每个踩过的坑都记录在代码注释与本文档中。

## 技术亮点

| 类别 | 内容 |
|------|------|
| I/O 模型 | epoll **ET** 边缘触发 + 非阻塞 I/O，readv + 64KB extrabuf 循环读，动态扩容 |
| 并发模型 | 主从 Reactor（主线程 accept + N 个 IO 线程 RR 分发）+ 有界队列工作线程池，eventfd 跨线程唤醒 |
| 生命周期 | `shared_ptr` + `queueInLoop` 延迟析构，消除并发下 use-after-free（ASan 验证） |
| HTTP/1.1 | GET/POST/HEAD，状态机解析（跨 TCP 拆包累积），keep-alive / pipelining 保序 |
| 协议安全 | CL+CL 冲突 → 400、Transfer-Encoding → 501 拒绝、头部/body 限长 → 413、请求行非法字符校验 |
| 路由 | 精确匹配 + 参数化（`/user/:id`）+ 通配符（`*`），URL 解码后匹配 |
| 静态文件 | sendfile 零拷贝（>64KB）+ LRU 内容缓存（≤64KB）+ realpath 路径穿越防护 + 304 协商缓存 |
| 背压 | 线程池队列满 → 503 **不关连接**（避免"拒绝→重连→更忙"风暴） |
| 定时器 | timerfd + 自实现最小堆（O(log n) cancel），空闲连接超时 |
| 优雅关闭 | 信号 → eventfd → 停止 accept → 排空在途请求 → 连接归零退出，10s 兜底 |
| 可观测性 | 6 个 lock-free atomic 指标 + `/stats` JSON，结构化日志，CLI/配置文件双源配置 |

## 架构

```
main
├── Config / SignalHandler / Logger / Metrics
├── Router + StaticFileHandler         # 业务层
├── ThreadPool                         # 工作线程（有界队列, 满 → 503 背压）
├── EventLoop (主线程)                 # accept + 信号 + 定时器
│   ├── TimerQueue (timerfd 最小堆)
│   ├── Acceptor (EMFILE 排空)
│   └── EventLoopThread × N            # 子事件循环（连接 I/O）
└── TcpConnection                      # shared_ptr 管理, 内含 Channel + Buffer×2 + HttpContext
```

一次请求的数据流：

```
epoll_wait → Channel → TcpConnection::handleRead (ET 循环读)
  → HttpContext::parseRequest (状态机, 跨拆包累积)
    → ThreadPool::tryRun (路由 + 静态文件)
      → queueInLoop (切回 IO 线程) → TcpConnection::send → send/sendfile
```

## 关键设计决策

每个决策的完整论证见 [docs/DESIGN.md](docs/DESIGN.md) 与 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

| 决策 | 一句话理由 |
|------|-----------|
| epoll ET 而非 LT | 事件只通知一次，必须循环读到 EAGAIN——减少 epoll_wait 返回次数，代价是实现更复杂 |
| `shared_ptr` 而非裸指针 | worker/定时器/IO 多线程并发持有连接引用，裸指针必然 UAF；最后一个引用释放才析构 |
| 延迟析构 | 事件回调执行时 Channel 指针还在 epoll 的 events 数组里，`delete this` 会悬垂 |
| realpath 而非字符串禁 `..` | 黑名单可被 `//`、`%2e%2e`、符号链接绕过；realpath 解析规范路径后前缀比较 |
| 大文件 sendfile / 小文件 LRU | >64KB 走零拷贝（内核态 DMA），≤64KB 走内存缓存（省 open/read syscall，实测 6.7 万 req/s） |
| 503 背压不关连接 | 关连接版会触发客户端"拒绝→重连→更忙"风暴 + 服务端 TIME_WAIT 堆积（实测 3000+） |
| 每连接串行化 | 保证流水线响应严格保序（RFC 7230 §6.3.2）；代价是单连接无并发，见 Roadmap ③ |
| `addTimer`/`cancel` 断言 IO 线程 | 定时器全生命周期单线程，零锁竞争 |

## 踩坑记录（调试故事）

每个问题都经过真实压测/工具复现，是面试"讲一个你解决的难题"的现成素材：

| 现象 | 根因 | 修复（验证手段） |
|------|------|------------------|
| 流水线请求响应错配 `[200,400] → [400,400]` | 多 worker 乱序完成 + queueInLoop 入队竞态 | 每连接请求串行化（压测复现） |
| 优雅关闭后连接永不退出 | 排空期静默丢弃请求，客户端挂起 | 排空期请求照常处理 + 强制 `Connection: close`（挂 3s 实测） |
| fd 耗尽时 epoll 忙循环 | accept 返回 EMFILE 但 listen fd 仍可读 | 预留 idle fd，EMFILE 时 accept 一个立即关闭排空 backlog（muduo 技巧） |
| 连接一建立就超时断开 | `resetTimer` 过期时间忘了加 `now`，立即到期 | 修复后 keep-alive 正常（功能回归） |
| ASan: heap-use-after-free WRITE | 子循环线程在 ThreadPool 析构后仍投递任务 | 固定拆除顺序：先停线程池，再关子循环（ASan 复现） |
| 最小堆堆序错乱导致 core | 0-based vector 混用 1-based 堆下标公式 | 修正 `(i-1)/2` / `2i+1`（core 复现） |
| chunked 请求产生连锁 400 | TE 头被静默忽略，帧字节被当新请求解析（走私向量） | TE → 501 显式拒绝（实测） |
| SIGTERM 后排空期请求无响应 | 在途请求被直接丢弃 | 排空状态机：请求照常处理，响应强制关连接（逐字节校验） |

## 快速开始

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/multireactor                 # 默认 8080, 4 IO + 4 worker, 静态目录 ./static

./build/multireactor -p 9090 -i 2 -w 8 -d ./public -t 30 --log-level DEBUG
./build/multireactor -c server.conf -p 9090   # CLI 优先级高于配置文件
./build/multireactor -h              # 全部选项
```

```bash
# 功能验证
curl http://localhost:8080/           # 200 Hello, World!
curl http://localhost:8080/user/42    # 200 参数路由
curl http://localhost:8080/stats      # 200 指标 JSON
curl -X POST http://localhost:8080/   # 405
curl -H "Content-Length: 104857600" -X POST http://localhost:8080/  # 413 提前拦截

# 压力测试
wrk -t4 -c100 -d10s --latency http://127.0.0.1:8080/
```

## 性能

测试环境：VMware 虚拟机 4 vCPU @ 3.2GHz（宿主机 Ryzen 7 7735H），4 IO + 4 worker，
Release，本机回环，wrk 5 轮 × 10s 取平均。

| 并发连接 | 平均吞吐 (req/s) | P50 | P99 |
|----------|------------------|-----|-----|
| 100 | 48,490 | 1.91 ms | 5.28 ms |
| 500 | 62,108 | 7.43 ms | 17.07 ms |
| 1000 | **67,003** | 14.04 ms | 31.39 ms |
| 2000 | 62,040 | 29.48 ms | 69.63 ms |

| 场景 | 吞吐 (req/s) | P50 | P99 |
|------|--------------|-----|-----|
| 动态路由 /user/123（-c1000） | 67,003 | 14.04 ms | 31.39 ms |
| 静态小文件（LRU 命中） | 67,429 | 1.30 ms | 4.51 ms |
| 大文件 300KB（sendfile） | 15,820 | 5.46 ms | 14.30 ms |

**诚实的瓶颈分析**（完整数据与推导见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)）：

- 每请求 **65,000 cycles**（20.4 µs），其中内核态 78%——每请求 7 次系统调用
  （readv / send / epoll_ctl×2 / timerfd_settime / eventfd×2），VM 内约 2.3 µs/次
- 4 vCPU 理论峰值 ≈ 19.7 万 req/s，实测达成率 34%：**延迟受限而非 CPU 受限**
  （1000 连接 × ~14ms ≈ 6.7 万，与 Little's law 一致）
- 吞吐随并发先升后降：串行化限制单连接并发，高吞吐靠多连接摊平（见 Roadmap ③）

## 测试与验证

- **协议/功能**：25 项协议测试 + 22 项功能回归全部通过
- **内存安全**：ASan/UBSan 下 300 并发混合畸形流量（并发 + 错误请求 + 静态文件）零错误
- **背压**：`--max-queue-size 1` + 200 并发实测触发 503，连接保持无重连风暴
- **优雅关闭**：SIGTERM 压测中在途大文件响应完整送达（逐字节校验）、活跃连接归零后 0.1s 内退出
- **CI**：GitHub Actions，gcc/clang × Release/Debug 四组矩阵构建 + 冒烟测试

## 当前边界与 Roadmap

诚实清单（面试中被问到"缺点"时，这比背稿更可信）：

- HTTP 仅 GET/POST/HEAD；无 chunked（TE → 501 显式拒绝）
- 无 TLS / HTTP/2 / WebSocket；路由不支持正则
- 指标无延迟分位数 histogram；日志直接写终端无异步落盘

**下一步计划（按优先级）**：

1. **单元测试接入**：HttpContext 解析状态机 / Router 是纯逻辑模块，接入轻量测试框架，
   覆盖拆包、畸形行、CL 冲突、TE 拒绝等边界（当前依赖冒烟测试，强度不足）
2. **HTTP/1.1 协议补齐**：Host 头校验（RFC 7230 §5.4）、响应 Date 头（RFC 7231 §7.1.1.2）、
   `Expect: 100-continue`、URL 解码按 RFC 3986 修正（path 中的 `+` 应保持字面量）
3. **打破串行化天花板**：请求级并行 + per-connection 有序响应队列替代 processing_ 串行化，
   单连接流水线吞吐有望成倍提升（当前架构最大瓶颈）
4. **syscall 削减**：合并 eventfd 两次投递、消除 epoll_ctl enable/disable 乒乓、
   timerfd 惰性重置——目标每请求 7 次 → 3 次，裸机部署可达数十万 req/s
5. **安全加固**：静态文件路径检查与 open 之间的 TOCTOU 窗口（openat2 或 O_NOFOLLOW + fstat）

## 项目结构

```
include/    # 18 个头文件：EventLoop / Channel / Acceptor / TcpConnection /
            # HttpContext / Buffer / TimerQueue / ThreadPool / Router / ...
src/        # 对应实现
docs/
├── ARCHITECTURE.md   # 架构详解 + 13 步迭代记录（每个优化对应功能增量）
└── DESIGN.md         # 逐模块设计决策 + 方案对比 + 底层原理（1119 行）
.github/workflows/ci.yml
```

## License

[MIT](LICENSE)

## 参考

- [muduo — 陈硕的 C++ 网络库](https://github.com/chenshuo/muduo)
- [The C10K Problem](http://www.kegel.com/c10k.html)
- Linux man: `epoll(7)`, `timerfd_create(2)`, `eventfd(2)`, `readv(2)`, `realpath(3)`, `sendfile(2)`
- RFC 7230（HTTP/1.1 消息语义与路由）、RFC 7231（方法/状态码）、RFC 3986（URI）
