# MultiReactor
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)]()

基于 **epoll ET** 从零实现的多线程 **Reactor 模式** C++17 HTTP 服务器，无任何第三方依赖。
核心设计参考 muduo（主从 Reactor、one loop per thread、eventfd 唤醒、timerfd 定时器），
实测动态路由峰值吞吐 **25.4 万 req/s**、流水线模式 **47.2 万 req/s**（8 vCPU VM，
本机回环，压测端与服务端同机——服务端自己还没跑满），静态小文件 10.2 万 req/s。

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

测试环境：VMware 虚拟机 **8 vCPU** @ 3.2GHz（宿主机 Ryzen 7 7735H，8 核 16 线程），
Release，本机回环，wrk 8 线程 10s；动态路由用 `-i 8 -w 1`，静态文件用 `-i 4 -w 2`
（两种负载的最优配置不同，见下）。**同一配置在不同时间窗内可差 ±20%**
（实测：`32×512` 一个窗口 435k–451k、另一个窗口 388k–399k），跨窗口比较请用
区间/中位数而不是单次值。

> 2026-09 补充：开环定速压测（wrk2，消除 coordinated omission）、流水线基准
> （自研客户端，逐响应字节级校验 + 服务端计数对账）与每请求 CPU 归因。完整数据、
> 方法论与脚本见 **[bench/README.md](bench/README.md)**。

| 并发连接 | 平均吞吐 (req/s) | P50 | P99 |
|----------|------------------|-----|-----|
| 100 | 194,503 / 195,109 | 0.42 ms | 1.16 ms |
| 500 | **253,957 / 254,489** | 1.67 ms | 4.75 ms |
| 1000 | 242,800 / 243,586 | 3.55 ms | 9.01 ms |
| 2000 | 231,590 / 230,367 | 7.72 ms | 17.49 ms |
| 5000 | 216,054 / 211,018 | 21.13 ms | 41.14 ms |

> 注：c=2000/5000 出现 503 背压（6.6% / 11.4%，线程池在途上限 1024 的设计内行为）。
> 在途上限是**全池单一计数器**（`ThreadPool::inFlight_`，判据 `inFlight_ >= maxQueueSize`），
> **与 worker 数无关**——`w=1` 和 `w=2` 的容量完全相同，调 worker 数不会改变 503 比例。
> 上表 503 比例高于旧 4 vCPU 表，是因为并发档位不同（本表测 `-c2000/5000`，旧表测
> `-c1000`），不能归因于 worker 数；要改容量请调 `--max-queue-size`。

| 场景 | 吞吐 (req/s) | P50 | P99 |
|------|--------------|-----|-----|
| 动态路由 /user/123（-c1000） | 242,800 | 3.55 ms | 9.01 ms |
| 静态小文件（LRU 命中，-c100） | 102,380 | 0.90 ms | 2.29 ms |
| 大文件 300KB（sendfile，-c100） | 34,509 | 1.84 ms | 4.47 ms |
| 大文件 1MB（sendfile，-c100） | 11,573 | 4.59 ms | 9.99 ms |
| 大文件 4MB（sendfile，-c100） | 2,805 | 18.29 ms | 250 ms |
| 静态 1KB 304 协商缓存（-c100） | 124,425 | 0.73 ms | — |

> 大文件行带宽：300KB ≈ 9.9 GB/s、1MB ≈ 11.3 GB/s、4MB ≈ 11.0 GB/s（loopback +
> page cache 命中，无网卡参与，**不是真实网络带宽**）。完整尺寸扫描（1KB–4MB，
> 含 LRU 缓存路径与 sendfile 路径的对比、304 路径、-c1000 档）见
> [bench/README.md](bench/README.md) 第 8 节。

**诚实的瓶颈分析**（完整数据与方法论见 [bench/README.md](bench/README.md) 第 9 节，
设计演化见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)）：

- **worker 池重构：共享队列 → one-loop-per-thread，-c1000 动态路由 +39%**：
  2026-08 将 ThreadPool 从"单共享队列 + mutex/condvar"改为"N 个 worker 各自运行
  独立 EventLoop，RR 分发 + eventfd 唤醒"——提交路径只剩两个原子操作（fetch_add
  在途计数 + RR 取模），无共享队列、无锁竞争、无 notify_one 空唤醒。同配置
  （4 IO + 2 worker）同档位对比：-c1000 101.2k → **140.4k**（+39%），-c100
  80.4k → 129.8k（+62%），P50 8.66 → 6.09 ms；503 比例从 1.2% 降至 0.5%；
  静态小文件基本持平（79.6k → 74.5k，-6% 在测量噪声边缘）
- **线程数没有普适最优，取决于负载（2026-09 修正，8 vCPU）**：动态路由
  `-i 8 -w 1` 最优（`w=1` 比 `w=2/3` 快 7%~15%）；静态小文件恰好相反，`w=2` 是
  `w=1` 的 **2.1 倍**（50.6k → 106.7k）——缓存文件的响应体拷贝在 worker 线程上做，
  而小文件路径每请求要 ~29µs CPU，单 worker 扛不住。旧结论"IO ≈ 核数，worker 取
  2~3"作废，改成按负载用 `bench/config_scan.sh` 测。
- **闭环数字由压测端决定，不是服务端**：峰值 254k 时服务端只用 3.05 核，同机的
  wrk 自己用了 **4.34 核**（16.5µs/请求，比服务端的 11.6µs 还贵），整机 92% 饱和。
  按每请求成本外推，服务端独占 8 核约 692k req/s——**这个数在本机测不出来**，
  必须把压测端移出这台 VM。继续突破的方向：空队列快路径（IO 线程直执行，砍掉
  跨线程 handoff）、调大 `max_queue_size`
- syscall 削减效果（历史记录，4+4 配置下测得）：削减前每请求 7 次系统调用
  （readv / send / epoll_ctl×2 / timerfd_settime / eventfd×2），削减后 ≈4 次；
  c=100 动态路由 48.5k→52.9k（+9%）、静态小文件 67.4k→80.4k（+19%，
  P50 1.30→1.06 ms）
- 延迟随并发近似线性，与 Little's law 一致（c=1000 时 242.8k × 平均延迟 4.1ms ≈
  1000 在途，P50 3.55ms）；c≥1000 三次复测波动 ±10%（VM 调度噪声）
- **开环定速压测（2026-09，wrk2）**：闭环只在响应返回后才发下个请求，排队时间被
  藏进客户端等待里。开环测量显示真实拐点在 **150k–200k**，饱和 ≈195k：
  150k 档 P50 仅 1.13ms，200k 档 P50 直接跳到 241ms。`-c100` 时吞吐 = 100 /
  端到端延迟，195k 对应单请求端到端 ≈512µs，而服务端纯服务时间只有 ~10µs——
  **这 500µs 就是跨线程 handoff 的固定开销**，也解释了为什么闭环 `-c500`
  能到 254k 而 `-c100` 只有 195k
- **请求级并行的单连接流水线收益已量化（2026-09）**：单连接批量发 K 个请求，
  K=1→1024 吞吐 **3.4k → 79.6k req/s（24 倍）**，每请求 279µs → 12.0µs；
  深度 ≤8 时批延迟恒定在 0.28–0.33ms，即存在约 280µs 的**固定批开销**，且
  **4 核与 8 核测得同一个值**——它是结构性的（IO 线程 → worker 的 eventfd 唤醒
  + 回投），正是"空队列快路径"要砍掉的那部分
- **流水线峰值 472k req/s**（32 连接 × 深度 1024，4 次复现 448k–472k），是闭环
  峰值的 1.86 倍、4 vCPU 峰值的 2.26 倍；此时**整机只有 67%**（服务端 3.79 核、
  压测端 1.54 核，还有 2.7 核空闲）——说明峰值卡在每 IO 线程的读-解析-提交-写回
  串行化与跨线程 hop 上，**不是算力**。4 个峰值批次 `mismatch=0`
- **背压比例由并发档位决定，与 worker 数无关（2026-09）**：`wrk -c3000` 15s 产生
  330,645 个 503（9.9%）、`-c5000` 390,732 个（11.8%）、`-c1000` 为 0；所有流水线
  配置（最高 49152 在途）均为 0——只有 >1024 并发非流水线连接才打得满在途上限。
  在途上限是全池单一计数器（`inFlight_`），`w=1` 与 `w=2` 容量相同

## 测试与验证

- **单元测试**：66 个用例（`./build/unit_tests` 或 `ctest`）覆盖 HttpContext 解析状态机（拆包逐字节、畸形请求行、CL 冲突、TE 拒绝、Host 校验、Expect、限长）、Router、StaticFileHandler（穿越/符号链接/缓存失效/304）、ThreadPool（RR 分发/在途有界/异常隔离/stop 后投递安全）、Date 头格式，零第三方依赖
- **端到端回归**：37 项（`python3 test/e2e.py`，或随 `ctest` 一并运行）——启动真实服务端进程、用裸 socket 发原始报文、逐字节校验，覆盖单测够不到的事件循环与连接生命周期：
  动态路由 + 协议行为（400/413/417/501/505 的触发条件与响应形态）+ 静态文件（LRU 缓存与 sendfile 两条路径的逐字节相等、304、路径穿越）+ 连接生命周期（keep-alive 复用、短连接关闭**计时**、`Connection: close`）+ 流水线保序（混合状态码不串位）+ 并发 50×20 + SIGTERM 排空（在途 4MB 响应完整送达、排空期请求照常服务）。仅依赖 Python 标准库
- **内存安全**：ASan/UBSan 下 66 单测 + 300 并发混合畸形流量（并发 + 错误请求 + 静态文件）零错误
- **背压**：`--max-queue-size 1` + 200 并发实测触发 503，连接保持无重连风暴
- **优雅关闭**：SIGTERM 时在途大文件响应完整送达（逐字节校验）、活跃连接归零后进程立即退出（远早于 10s 兜底 deadline；`test/e2e.py` 的 `ShutdownExitsPromptly` 逐轮计时守住该回归）

## 当前边界

- HTTP 仅 GET/POST/HEAD；无 chunked（TE → 501 显式拒绝）
- 无 TLS / HTTP/2 / WebSocket；路由不支持正则
- 指标无延迟分位数 histogram；日志直接写终端无异步落盘

## 项目结构

```
include/    # 19 个头文件：EventLoop / Channel / Acceptor / TcpConnection /
            # HttpContext / Buffer / TimerQueue / ThreadPool / Router / ...
src/        # 对应实现
test/       # 66 个单元测试（零依赖 TEST_CASE 框架）+ e2e.py（37 项端到端回归），均接入 ctest
bench/      # 性能测试脚本 + 实测数据（开环延迟扫描 / 流水线基准 / 拆包测试）
docs/
├── ARCHITECTURE.md   # 架构详解 + 17 步迭代记录（每个优化对应功能增量）
└── DESIGN.md         # 逐模块设计决策 + 方案对比 + 底层原理（595 行）
```

## License

[MIT](LICENSE)

## 参考

- [muduo — 陈硕的 C++ 网络库](https://github.com/chenshuo/muduo)
- [The C10K Problem](http://www.kegel.com/c10k.html)
- Linux man: `epoll(7)`, `timerfd_create(2)`, `eventfd(2)`, `readv(2)`, `realpath(3)`, `sendfile(2)`
- RFC 7230（HTTP/1.1 消息语义与路由）、RFC 7231（方法/状态码）、RFC 3986（URI）
