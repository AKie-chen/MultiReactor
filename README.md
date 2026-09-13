# MultiReactor
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)]()

基于 **epoll ET** 从零实现的多线程 **Reactor 模式** C++17 HTTP 服务器，无任何第三方依赖。
核心设计参考 muduo（主从 Reactor、one loop per thread、eventfd 唤醒、timerfd 定时器），
实测动态路由峰值吞吐 **14.0 万 req/s**（4 IO + 2 worker，VM 环境），静态小文件 7.5 万 req/s。

> 这是一个以学习为目的、以生产工程标准要求自己的项目：所有关键设计决策都经过
> 真实压测/ASan/UBSan 验证，每个踩过的坑都记录在代码注释与本文档中。

## 技术亮点

| 类别 | 内容 |
|------|------|
| I/O 模型 | epoll **ET** 边缘触发 + 非阻塞 I/O，readv + 64KB extrabuf 循环读，动态扩容 |
| 并发模型 | 主从 Reactor（主线程 accept + N 个 IO 线程 RR 分发）+ one-loop-per-thread 工作线程池（每 worker 独立 EventLoop，无共享队列，在途计数有界），eventfd 跨线程唤醒；**请求级并行**：同连接多请求独立提交 worker，seq 有序响应队列按序重排发送 |
| 生命周期 | `shared_ptr` + `queueInLoop` 延迟析构，消除并发下 use-after-free（ASan 验证） |
| HTTP/1.1 | GET/POST/HEAD，状态机解析（跨 TCP 拆包累积），keep-alive / pipelining 保序；Host 头校验（RFC 7230 §5.4）、响应 Date 头（RFC 7231 §7.1.1.2，每秒缓存省序列化开销）、`Expect: 100-continue`、URL 解码按 RFC 3986（path 中 `+` 保持字面量，仅 query 做表单解码） |
| 协议安全 | 缺 Host / 重复 Host 冲突 → 400、CL+CL 冲突 → 400、Transfer-Encoding → 501 拒绝、无法满足的 Expect → 417、头部/body 限长 → 413（先于 100-continue） |
| 路由 | 精确匹配 + 参数化（`/user/:id`）+ 通配符（`*`），URL 解码后匹配 |
| 静态文件 | sendfile 零拷贝（>64KB）+ LRU 内容缓存（≤64KB）+ realpath 路径穿越防护 + 304 协商缓存；**TOCTOU 加固**：openat2 + RESOLVE_NO_SYMLINKS 关闭校验与 open 之间的竞态窗口，文件 fd 一次打开直传发送层（消除二次 open） |
| 背压 | 线程池队列满 → 503 **不关连接**（避免"拒绝→重连→更忙"风暴） |
| 异常隔离 | 三层 try/catch：handler 异常 → 500 响应；逃逸异常记日志不杀线程，在途计数不泄漏（优雅关闭不挂 10s） |
| 定时器 | timerfd + 自实现最小堆（O(log n) cancel），空闲连接超时 |
| 优雅关闭 | 信号 → eventfd → 停止 accept → 排空在途请求 → 连接归零退出，10s 兜底 |
| 可观测性 | 6 个 lock-free atomic 指标 + `/stats` JSON，结构化日志，CLI/配置文件双源配置 |
| syscall 削减 | eventfd 唤醒去重（`wakeupPending_` 原子）、send 先直接写 EAGAIN 才注册 EPOLLOUT（消除 epoll_ctl 乒乓）、timerfd 惰性重置（1s 心跳扫描替代每请求 cancel/addTimer）、Date 头每秒缓存。strace 实测 1000 keep-alive 请求：epoll_ctl 11 次、timerfd_settime 5 次（旧实现各 2000/1000 次），每请求 ≈4 次 syscall |

## 架构

```
main
├── Config / SignalHandler / Logger / Metrics
├── Router + StaticFileHandler         # 业务层
├── ThreadPool                         # 工作线程（one loop per thread, 在途有界 → 503 背压）
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
| 请求级并行 + seq 有序响应队列 | 流水线请求独立提交 worker（乱序完成），`deliverResponse` 按 seq 直发/暂存/顺藤排空，保序（RFC 7230 §6.3.2）的同时打掉单连接串行化天花板；`markForClose` 先于投递评估，direct 写路径也能及时关闭短连接 |
| openat2 而非仅 realpath 校验 | realpath 只消除"已有"符号链接，校验与 open 之间的替换窗口是 TOCTOU；openat2 + RESOLVE_NO_SYMLINKS 在 open 时刻原子校验全路径分量，老内核回退 O_NOFOLLOW + fstat dev/ino 比对（残余窗口仅中间分量，见代码注释） |
| 大文件 sendfile / 小文件 LRU | >64KB 走零拷贝（内核态 DMA），≤64KB 走内存缓存（省 open/read syscall，实测静态小文件 8 万 req/s） |
| 503 背压不关连接 | 关连接版会触发客户端"拒绝→重连→更忙"风暴 + 服务端 TIME_WAIT 堆积（实测 3000+） |
| `addTimer`/`cancel` 断言 IO 线程 | 定时器全生命周期单线程，零锁竞争 |
| 三层异常隔离 | 单请求异常不得杀死进程：handler 异常转 500，逃逸异常记日志、在途计数不泄漏（否则排空永不收敛）、线程不退出 |
| Date 头每秒缓存 | RFC 7231 日期粒度是秒，同秒内所有响应复用同一格式化结果，省每响应一次 `gmtime_r`+`strftime`（thread_local 无锁） |

## 踩坑记录（调试故事）

| 现象 | 根因 | 修复（验证手段） |
|------|------|------------------|
| 流水线请求响应错配 `[200,400] → [400,400]` | 多 worker 乱序完成 + queueInLoop 入队竞态 | 分配 seq + `deliverResponse` 暂存/补发按序重排（压测复现） |
| 短连接响应后不关闭（挂到心跳超时） | 直接写成功路径无 EPOLLOUT 事件，`maybeCloseAfterSend` 唯一评估点被跳过；`markForClose` 又在投递之后才设置标志 | `markForClose` 先于 `deliverResponse`，末尾评估可见（EOF 时序回归脚本复现：修复前每连接 3s 超时） |
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
./build/multireactor                 # 默认 8080, 4 IO + 2 worker, 静态目录 ./static

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

测试环境：VMware 虚拟机 4 vCPU @ 3.2GHz（宿主机 Ryzen 7 7735H），4 IO + 2 worker
（默认配置），Release，本机回环，wrk 4 线程 10s（2026-08 worker 池重构为
one-loop-per-thread 后重新压测；c≥1000 多次运行波动 ±5%，表中为 2~3 轮平均）。

> 2026-09 补充：开环定速压测（wrk2，消除 coordinated omission）与流水线基准
> （自研客户端，逐响应字节级校验 + 服务端计数对账）。完整数据、方法论与脚本见
> **[bench/README.md](bench/README.md)**。

| 并发连接 | 平均吞吐 (req/s) | P50 | P99 |
|----------|------------------|-----|-----|
| 100 | 129,814 | 0.67 ms | 2.04 ms |
| 500 | 136,646 | 3.16 ms | 9.24 ms |
| 1000 | **140,445** | 6.09 ms | 15.15 ms |
| 2000 | 129,197 | 13.59 ms | 28.07 ms |
| 5000 | 108,348 | 40.31 ms | 84.96 ms |

> 注：c=2000/5000 时出现约 0.5% 的 503（线程池在途上限 1024 的背压响应，设计内行为，见下）。

| 场景 | 吞吐 (req/s) | P50 | P99 |
|------|--------------|-----|-----|
| 动态路由 /user/123（-c1000） | 140,445 | 6.09 ms | 15.15 ms |
| 静态小文件（LRU 命中，-c100） | 74,480 | 1.21 ms | 3.34 ms |
| 大文件 300KB（sendfile，-c100） | 19,171 | 3.55 ms | 9.24 ms |
| 大文件 1MB（sendfile，-c100） | 7,351 | 7.53 ms | 20.25 ms |
| 大文件 4MB（sendfile，-c100） | 1,954 | 27.50 ms | 215 ms |
| 静态 1KB 304 协商缓存（-c100） | 75,082 | 1.20 ms | 3.42 ms |

> 大文件行带宽：300KB ≈ 5.5 GB/s、1MB ≈ 7.2 GB/s、4MB ≈ 7.7 GB/s（loopback +
> page cache 命中，无网卡参与，**不是真实网络带宽**）。完整尺寸扫描（1KB–4MB，
> 含 LRU 缓存路径与 sendfile 路径的对比、304 路径、-c1000 档）见
> [bench/README.md](bench/README.md) 第 8 节。

**诚实的瓶颈分析**（完整数据与推导见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)）：

- **worker 池重构：共享队列 → one-loop-per-thread，-c1000 动态路由 +39%**：
  2026-08 将 ThreadPool 从"单共享队列 + mutex/condvar"改为"N 个 worker 各自运行
  独立 EventLoop，RR 分发 + eventfd 唤醒"——提交路径只剩两个原子操作（fetch_add
  在途计数 + RR 取模），无共享队列、无锁竞争、无 notify_one 空唤醒。同配置
  （4 IO + 2 worker）同档位对比：-c1000 101.2k → **140.4k**（+39%），-c100
  80.4k → 129.8k（+62%），P50 8.66 → 6.09 ms；503 比例从 1.2% 降至 0.5%；
  静态小文件基本持平（79.6k → 74.5k，-6% 在测量噪声边缘）
- **线程数结论不变**：重构后重扫 4+4（新架构，-c1000 98.3k），仍显著低于 4+2
  的 140.4k——4 核上 worker 过多只是超订抢占，与锁实现无关；规则仍为
  **IO ≈ 核数，worker 取 2~3**（多核机器需重测）
- 当前吞吐平台 ≈14 万 req/s，由 2 个 worker 各自的 loop 消费速率决定（每 worker
  ~7 万 tasks/s），而非 CPU（4 vCPU 理论峰值 ≈ 19.7 万，达成率 71%）。
  c=2000/5000 的 503 即突发提交溢出 1024 在途上限的背压。继续突破的方向：
  空队列快路径（IO 线程直执行，砍掉跨线程 handoff）、调大 `max_queue_size`
- syscall 削减效果（历史记录，4+4 配置下测得）：削减前每请求 7 次系统调用
  （readv / send / epoll_ctl×2 / timerfd_settime / eventfd×2），削减后 ≈4 次；
  c=100 动态路由 48.5k→52.9k（+9%）、静态小文件 67.4k→80.4k（+19%，
  P50 1.30→1.06 ms）
- 延迟随并发近似线性（1000 连接 × 6.1 ms ≈ 16 万，与 Little's law 量级一致）
- c≥1000 三次复测波动 ±10%（VM 调度噪声）
- **开环定速压测（2026-09，wrk2）**：闭环只在响应返回后才发下个请求，排队时间被
  藏进客户端等待里。开环测量显示真实拐点在 **110k–120k**，饱和 ≈123k（与闭环
  -c1000 的 126k 相互印证）；80k 负载下 P50 仅 1.34ms、P99 3.6ms；90k 起 P99.9
  从 4.5ms 恶化到 39ms 而 P50 平稳——**尾部先崩**，只看均值/P50 会漏掉退化起点
- **请求级并行的单连接流水线收益已量化（2026-09）**：单连接批量发 K 个请求，
  K=1→1024 吞吐 **3.3k → 63.5k req/s（19 倍）**，每请求 284µs → 15.2µs；
  深度 ≤8 时批延迟恒定 0.28ms，即存在约 280µs 的**固定批开销**（IO 线程 →
  worker 的 eventfd 唤醒 + 回投），正是"空队列快路径"要砍掉的那部分
- **流水线峰值 208.8k req/s**（16 连接 × 深度 256），比闭环最好成绩高 66%，
  超过 4 vCPU 理论推算上限——worker 池重构后 CPU 不再是唯一瓶颈；
  16×128 与 128×64 两种差异极大的客户端配置收敛到同一水平，指向服务端天花板
- **背压复现（2026-09）**：`wrk -c3000` 15s 产生 5,764 个 503（0.34%），
  `-c1000` 为 0；所有流水线配置（最高 8192 在途）均为 0——只有 >1024 并发
  非流水线连接才打得满在途上限

## 测试与验证

- **单元测试**：66 个用例（`./build/unit_tests` 或 `ctest`）覆盖 HttpContext 解析状态机（拆包逐字节、畸形请求行、CL 冲突、TE 拒绝、Host 校验、Expect、限长）、Router、StaticFileHandler（穿越/符号链接/缓存失效/304）、ThreadPool（RR 分发/在途有界/异常隔离/stop 后投递安全）、Date 头格式，零第三方依赖
- **协议/功能**：27 项端到端回归（动态路由 + 协议行为 + 静态文件逐字节 + 短连接关闭时序 + 并发 50×20 + SIGTERM 优雅关闭）全部通过
- **内存安全**：ASan/UBSan 下 64 单测 + 300 并发混合畸形流量（并发 + 错误请求 + 静态文件）零错误
- **背压**：`--max-queue-size 1` + 200 并发实测触发 503，连接保持无重连风暴
- **优雅关闭**：SIGTERM 压测中在途大文件响应完整送达（逐字节校验）、活跃连接归零后 0.1s 内退出
- **CI**：GitHub Actions，gcc/clang × Release/Debug 四组矩阵构建 + 冒烟测试

## 当前边界

- HTTP 仅 GET/POST/HEAD；无 chunked（TE → 501 显式拒绝）
- 无 TLS / HTTP/2 / WebSocket；路由不支持正则
- 指标无延迟分位数 histogram；日志直接写终端无异步落盘

## 项目结构

```
include/    # 19 个头文件：EventLoop / Channel / Acceptor / TcpConnection /
            # HttpContext / Buffer / TimerQueue / ThreadPool / Router / ...
src/        # 对应实现
test/       # 66 个单元测试（零依赖 TEST_CASE 框架，ctest 接入）
bench/      # 性能测试脚本 + 实测数据（开环延迟扫描 / 流水线基准 / 拆包测试）
docs/
├── ARCHITECTURE.md   # 架构详解 + 17 步迭代记录（每个优化对应功能增量）
└── DESIGN.md         # 逐模块设计决策 + 方案对比 + 底层原理（595 行）
.github/workflows/ci.yml
```

## License

[MIT](LICENSE)

## 参考

- [muduo — 陈硕的 C++ 网络库](https://github.com/chenshuo/muduo)
- [The C10K Problem](http://www.kegel.com/c10k.html)
- Linux man: `epoll(7)`, `timerfd_create(2)`, `eventfd(2)`, `readv(2)`, `realpath(3)`, `sendfile(2)`
- RFC 7230（HTTP/1.1 消息语义与路由）、RFC 7231（方法/状态码）、RFC 3986（URI）
