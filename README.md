# MultiReactor
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)]()

基于 **epoll ET** 从零实现的多线程 **Reactor 模式** C++17 HTTP 服务器，无任何第三方依赖。
核心设计参考 muduo（主从 Reactor、one loop per thread、eventfd 唤醒、timerfd 定时器），
实测动态路由峰值吞吐 **10.1 万 req/s**（4 IO + 2 worker，VM 环境），静态小文件 8 万 req/s。

> 这是一个以学习为目的、以生产工程标准要求自己的项目：所有关键设计决策都经过
> 真实压测/ASan/UBSan 验证，每个踩过的坑都记录在代码注释与本文档中。

## 技术亮点

| 类别 | 内容 |
|------|------|
| I/O 模型 | epoll **ET** 边缘触发 + 非阻塞 I/O，readv + 64KB extrabuf 循环读，动态扩容 |
| 并发模型 | 主从 Reactor（主线程 accept + N 个 IO 线程 RR 分发）+ 有界队列工作线程池，eventfd 跨线程唤醒；**请求级并行**：同连接多请求独立提交 worker，seq 有序响应队列按序重排发送 |
| 生命周期 | `shared_ptr` + `queueInLoop` 延迟析构，消除并发下 use-after-free（ASan 验证） |
| HTTP/1.1 | GET/POST/HEAD，状态机解析（跨 TCP 拆包累积），keep-alive / pipelining 保序；Host 头校验（RFC 7230 §5.4）、响应 Date 头（RFC 7231 §7.1.1.2）、`Expect: 100-continue`、URL 解码按 RFC 3986（path 中 `+` 保持字面量，仅 query 做表单解码） |
| 协议安全 | 缺 Host / 重复 Host 冲突 → 400、CL+CL 冲突 → 400、Transfer-Encoding → 501 拒绝、无法满足的 Expect → 417、头部/body 限长 → 413（先于 100-continue） |
| 路由 | 精确匹配 + 参数化（`/user/:id`）+ 通配符（`*`），URL 解码后匹配 |
| 静态文件 | sendfile 零拷贝（>64KB）+ LRU 内容缓存（≤64KB）+ realpath 路径穿越防护 + 304 协商缓存；**TOCTOU 加固**：openat2 + RESOLVE_NO_SYMLINKS 关闭校验与 open 之间的竞态窗口，文件 fd 一次打开直传发送层（消除二次 open） |
| 背压 | 线程池队列满 → 503 **不关连接**（避免"拒绝→重连→更忙"风暴） |
| 定时器 | timerfd + 自实现最小堆（O(log n) cancel），空闲连接超时 |
| 优雅关闭 | 信号 → eventfd → 停止 accept → 排空在途请求 → 连接归零退出，10s 兜底 |
| 可观测性 | 6 个 lock-free atomic 指标 + `/stats` JSON，结构化日志，CLI/配置文件双源配置 |
| syscall 削减 | eventfd 唤醒去重（`wakeupPending_` 原子）、send 先直接写 EAGAIN 才注册 EPOLLOUT（消除 epoll_ctl 乒乓）、timerfd 惰性重置（1s 心跳扫描替代每请求 cancel/addTimer）。strace 实测 1000 keep-alive 请求：epoll_ctl 11 次、timerfd_settime 5 次（旧实现各 2000/1000 次），每请求 ≈4 次 syscall |

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
| 请求级并行 + seq 有序响应队列 | 流水线请求独立提交 worker（乱序完成），`deliverResponse` 按 seq 直发/暂存/顺藤排空，保序（RFC 7230 §6.3.2）的同时打掉单连接串行化天花板；`markForClose` 先于投递评估，direct 写路径也能及时关闭短连接 |
| openat2 而非仅 realpath 校验 | realpath 只消除"已有"符号链接，校验与 open 之间的替换窗口是 TOCTOU；openat2 + RESOLVE_NO_SYMLINKS 在 open 时刻原子校验全路径分量，老内核回退 O_NOFOLLOW + fstat dev/ino 比对（残余窗口仅中间分量，见代码注释） |
| 大文件 sendfile / 小文件 LRU | >64KB 走零拷贝（内核态 DMA），≤64KB 走内存缓存（省 open/read syscall，实测静态小文件 8 万 req/s） |
| 503 背压不关连接 | 关连接版会触发客户端"拒绝→重连→更忙"风暴 + 服务端 TIME_WAIT 堆积（实测 3000+） |
| `addTimer`/`cancel` 断言 IO 线程 | 定时器全生命周期单线程，零锁竞争 |

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
（默认配置，2026-08 线程数调整后重新压测），Release，本机回环，wrk 4 线程 10s
（c≥1000 多次运行波动 ±5%，表中为 2~3 轮平均）。

| 并发连接 | 平均吞吐 (req/s) | P50 | P99 |
|----------|------------------|-----|-----|
| 100 | 80,374 | 1.09 ms | 3.53 ms |
| 500 | 100,949 | 4.34 ms | 12.68 ms |
| 1000 | **101,223** | 8.66 ms | 21.89 ms |
| 2000 | 101,585 | 17.65 ms | 38.98 ms |
| 5000 | 85,245 | 54.98 ms | 103.46 ms |

> 注：c=2000/5000 时出现约 1% 的 503（线程池队列满的背压响应，设计内行为，见下）。

| 场景 | 吞吐 (req/s) | P50 | P99 |
|------|--------------|-----|-----|
| 动态路由 /user/123（-c1000） | 101,223 | 8.66 ms | 21.89 ms |
| 静态小文件（LRU 命中，-c100） | 79,560 | 1.12 ms | 2.74 ms |
| 大文件 300KB（sendfile，-c100） | 19,665 | 4.04 ms | 10.37 ms |

**诚实的瓶颈分析**（完整数据与推导见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)）：

- **线程数是压测出来的，不是拍脑袋定的**：2026-08 对 4 核机器做 IO/worker 全组合
  扫描——4+4（旧默认）6.0 万 → 2+2 8.0 万 → 3+2 9.2 万 → **4+2 10.1 万 req/s**；
  2+2 加 worker 到 2+3 反而降到 6.4 万。原因：任务极轻（微秒级）时 worker 过多
  放大线程池锁竞争与 notify_one 空唤醒（4+4 压测期间上下文切换 25.3 万次，
  2+2 仅 15.7 万次），而 IO 线程是请求生命周期的主线，越多越平。
  规则：**IO ≈ 核数，worker 取 2~3**（多核机器需重测）
- 当前吞吐平台 ≈10.1 万 req/s，恰好 ≈ **2 个 worker × ~5 万 tasks/s** 的消费速率：
  平台由线程池吞吐决定，而非 CPU（4 vCPU 理论峰值 ≈ 19.7 万，达成率 51%）。
  c=2000/5000 的 503 即提交突发溢出 1024 队列的背压。突破平台的方向：
  空队列快路径（IO 线程直执行，砍掉跨线程 handoff），或按并发档位调
  `max_queue_size` / `worker_threads`
- syscall 削减效果（历史记录，4+4 配置下测得）：削减前每请求 7 次系统调用
  （readv / send / epoll_ctl×2 / timerfd_settime / eventfd×2），削减后 ≈4 次；
  c=100 动态路由 48.5k→52.9k（+9%）、静态小文件 67.4k→80.4k（+19%，
  P50 1.30→1.06 ms）
- 延迟随并发近似线性（1000 连接 × 8.7 ms ≈ 11.4 万，与 Little's law 量级一致）
- c≥1000 三次复测波动 ±10%（VM 调度噪声）；请求级并行的单连接流水线收益需专用
  流水线压测工具量化（wrk 单连接不并发，现有多连接数据反映的是聚合吞吐）

## 测试与验证

- **单元测试**：60 个用例（`./build/unit_tests` 或 `ctest`）覆盖 HttpContext 解析状态机（拆包逐字节、畸形请求行、CL 冲突、TE 拒绝、Host 校验、Expect、限长）、Router、StaticFileHandler（穿越/符号链接/缓存失效/304），零第三方依赖
- **协议/功能**：27 项端到端回归（动态路由 + 协议行为 + 静态文件逐字节 + 短连接关闭时序 + 并发 50×20 + SIGTERM 优雅关闭）全部通过
- **内存安全**：ASan/UBSan 下 60 单测 + 300 并发混合畸形流量（并发 + 错误请求 + 静态文件）零错误
- **背压**：`--max-queue-size 1` + 200 并发实测触发 503，连接保持无重连风暴
- **优雅关闭**：SIGTERM 压测中在途大文件响应完整送达（逐字节校验）、活跃连接归零后 0.1s 内退出
- **CI**：GitHub Actions，gcc/clang × Release/Debug 四组矩阵构建 + 冒烟测试

## 当前边界

- HTTP 仅 GET/POST/HEAD；无 chunked（TE → 501 显式拒绝）
- 无 TLS / HTTP/2 / WebSocket；路由不支持正则
- 指标无延迟分位数 histogram；日志直接写终端无异步落盘

## 项目结构

```
include/    # 18 个头文件：EventLoop / Channel / Acceptor / TcpConnection /
            # HttpContext / Buffer / TimerQueue / ThreadPool / Router / ...
src/        # 对应实现
test/       # 60 个单元测试（零依赖 TEST_CASE 框架，ctest 接入）
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
