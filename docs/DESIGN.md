# MultiReactor — 逐模块设计决策与底层原理

本文档逐模块拆解 MultiReactor 的设计：每个模块的**职责**、**关键设计决策**、
**候选方案对比**（为什么选 A 不选 B）与涉及的**底层原理**。架构全貌与迭代
历史见 [docs/ARCHITECTURE.md](ARCHITECTURE.md)。

---

## 目录

1. [Config — 配置系统](#config--配置系统)
2. [Logger — 结构化日志](#logger--结构化日志)
3. [EventLoop + Channel — 事件循环](#eventloop--channel--事件循环)
4. [Acceptor — 连接接入](#acceptor--连接接入)
5. [Buffer — 缓冲区](#buffer--缓冲区)
6. [TcpConnection — 连接生命周期与保序发送](#tcpconnection--连接生命周期与保序发送)
7. [HttpContext — 请求解析状态机](#httpcontext--请求解析状态机)
8. [HttpRequest / HttpResponse — 报文数据模型](#httprequest--httpresponse--报文数据模型)
9. [Router — 路由](#router--路由)
10. [StaticFileHandler — 静态文件服务](#staticfilehandler--静态文件服务)
11. [ThreadPool — 工作线程池](#threadpool--工作线程池)
12. [TimerQueue — 定时器](#timerqueue--定时器)
13. [SignalHandler — 信号处理与优雅关闭](#signalhandler--信号处理与优雅关闭)
14. [Metrics — 指标](#metrics--指标)
15. [TcpServer — 服务器组装](#tcpserver--服务器组装)
16. [跨模块设计：异常隔离](#跨模块设计异常隔离)
17. [跨模块设计：背压与保序](#跨模块设计背压与保序)
18. [跨模块设计：syscall 削减](#跨模块设计syscall-削减)

---

## Config — 配置系统

**职责**：把 CLI 参数与 key=value 配置文件统一解析进 `ServerConfig` 结构体。

### 关键设计决策

- **两遍扫描、CLI 优先**：先解析配置文件，再解析命令行——后写覆盖先写，天然实现
  "CLI 优先级高于配置文件"。避免引入"配置文件里标记哪些键被 CLI 覆盖"的状态。
- **单一数据源**：解析结果直接写入 `ServerConfig`（纯数据 struct），运行期所有模块
  只读这份配置，不存在"半初始化"或"多个配置来源互相打架"的中间状态。
- **连接数上限只放配置文件**：`max_connections` 是最危险的旋钮（拧错直接拒客），
  刻意不提供 CLI 短参数，防止误操作。

### 方案对比

| 方案 | 评价 |
|------|------|
| 两遍扫描（本实现） | 逻辑简单、覆盖语义清晰；代价是配置项少时显冗余 |
| 第三方差 (getopt + ini 库) | 项目原则零第三方依赖；getopt 行为各平台不一致 |
| 环境变量 | 隐式、难发现，不适合作为服务配置入口 |

---

## Logger — 结构化日志

**职责**：多级别、带时间戳与源码位置的日志输出。

### 关键设计决策

- **`__FILE__`/`__LINE__` 编译期捕获**：`LOG_INFO << ...` 宏展开为构造临时
  `LogStream` 对象，析构时落盘。流式语法允许 `<<` 任意可打印类型，避免
  printf 家族的类型不安全。
- **行缓冲替代手动 flush**：stdout 在终端下行缓冲，`\n` 自动刷；显式 flush
  反而打断 stdio 与内核的批量写入（Step 11 修复，省掉每行一次系统调用）。
- **5 级过滤**：`DEBUG < INFO < WARN < ERROR`，级别在编译后运行期决定
  （`Logger::setLevel`），压测时默认只落 WARN 以上，避免日志成为瓶颈。

### 底层原理

`std::ostream` 的 `operator<<` 重载链：每个日志语句构造一个临时对象，
析构即"提交"。日志线程安全由"每行一次 `<<` 链 + 单次输出"保证，多线程
交叉写行的代价是行内字符可能交错——对服务日志可接受，未引入锁。

---

## EventLoop + Channel — 事件循环

**职责**：封装 epoll 等待/分发；`Channel` 把一个 fd + 关注事件 + 回调绑定成
epoll 数据（`data.ptr`），是"事件 → 处理"的翻译层。

### 关键设计决策

- **one loop per thread**：每个 EventLoop 绑定一个线程（`threadId_` 记录），
  回调、定时器、pending functors 全部单线程执行。`assert(isInLoopThread())`
  把"跨线程触碰"变成构建期即暴露的崩溃而非难以排查的竞态。
- **`wakeupPending_` 唤醒去重**：跨线程投递 `queueInLoop` 时，只有
  `exchange(true)` 返回 false 的投递者才真正写 eventfd——N 个投递者并发时
  只有第一个付出 write 系统调用，其余被原子去重。接收侧 do-while 处理到
  pending 为空才解除武装（`wakeupPending_ = false`），否则"处理期间新入队"
  的任务会丢掉唤醒（单遍处理会漏：投递者看到置位跳过 write）。
- **`callingPendingFunctors_` 防死锁自唤醒**：正在执行 pending functors 的
  线程再次 `queueInLoop` 不需要 eventfd 唤醒自己（本迭代马上会再检查）。
- **事件分发异常隔离**：`channel->handleEvent()` 与每个 pending functor 都包
  在 try/catch 里（Step 17）——单连接回调抛异常记日志后继续，IO 线程不退出；
  catch 在循环**内**，漏掉一个回调不耽误后续就绪事件处理；`callingPendingFunctors_`
  复位在 catch 之外，不破坏唤醒语义。
- **`events_` 动态扩容**：epoll_wait 返回数 == 数组大小时翻倍——一次事件风暴
  只多一次 epoll_ctl，不丢事件（Step 10 修复）。

### 方案对比

| 方案 | 评价 |
|------|------|
| epoll LT | 实现简单，但每次事件都要"处理到无事可做"的循环，epoll_wait 返回次数多，且容易误加忙等 |
| epoll ET（本实现） | 事件只通知一次，必须循环读到 EAGAIN；配合 readv + extrabuf 一次读满，减少 wakeup 次数（README 性能节实测 syscall 削减的根源之一） |
| poll / select | O(n) 扫描 + fd 上限，连接数上万时不可用 |

### 底层原理

- `eventfd` 的 EFD_NONBLOCK + 计数累加语义：读一次只清空一次计数，多次 write
  可被一次 read 吸收；ET 模式要求读到 EAGAIN 才算清空。
- `data.ptr` 直存 `Channel*`：epoll 事件返回时 O(1) 定位对象，无 map 查找。
- `epoll_ctl` MOD vs ADD：`added_` 标志决定操作类型，避免对未注册 fd 调用 MOD
  返回 ENOENT。

---

## Acceptor — 连接接入

**职责**：封装 listen fd，accept 新连接并分发给 IO 子循环。

### 关键设计决策

- **RR 分发**：`next_++ % subLoops_.size()` 把新连接轮流分给各 IO 线程——
  无锁（单线程 accept），连接近似均匀分布。
- **SO_REUSEADDR + TCP_NODELAY + SO_KEEPALIVE**：重启免 TIME_WAIT 等待；
  小报文免 Nagle 延迟（HTTP 响应头通常 < MSS，Nagle 会憋 40ms）；服务端
  KEEPALIVE 兜底探测死对端。
- **EMFILE 排空**（Step 8/踩坑记录）：fd 耗尽时 accept 返回 EMFILE，但 listen
  fd 仍可读 → 若不做处理，epoll 会忙循环。预留一个 idle fd，EMFILE 时把它
  `dup` 出去 accept 一个连接再立刻关闭——把 backlog 里的连接消费掉，让对端
  的 connect 成功返回（而不是无限重试），同时释放该连接的 fd。
- **连接数上限检查在 accept 回调内**：`connectionCount_ >= maxConnections_` 时
  直接 close，不进入连接管理（atomic 计数 + mutex 保护的集合）。

### 方案对比

| 方案 | 评价 |
|------|------|
| 主线程 accept + 每个连接一把线程 | 线程爆炸，无收益 |
| 主线程 accept + 子循环分发（本实现） | 主从 Reactor 标准形态；accept 本身不是瓶颈（4 核下实测 ≤ 5% 开销） |
| SO_REUSEPORT 多进程 | 超出本项目范围（单进程多线程）；需内核 ≥ 3.9，且引入跨进程连接均衡问题 |

---

## Buffer — 缓冲区

**职责**：连接级读/写缓冲，支持 readv 分散读、prependable 区、指数扩容。

### 关键设计决策

- **prependable 三区模型**（[prependable | readable | writeable]）：
  `readIndex_` 之前的空间可在 O(1) 内 prepend——解析器"回退"或写入 4 字节长度
  前缀等场景不用整体搬移数据（Step 10 修复：原实现 prepend 是 O(n) 移位）。
- **readv + 64KB extrabuf**：`readv` 一次读入 [缓冲区剩余空间 | 栈上 64KB]，
  内核把两次 read 的拷贝合成一次系统调用；ET 循环读到 EAGAIN，兼顾吞吐与
  内存（连接闲置时缓冲区不膨胀）。
- **指数扩容**：`append` 空间不足时容量翻倍（cap×2）——摊还 O(1)，避免
  线性增长策略的频繁 realloc（Step 11 修复）。
- **`shrinkIfLarge` 缩容**：大响应（如 POST body 16MB）消费完后退还内存，
  防止长连接把峰值内存焊死在缓冲区里。

### 方案对比

| 方案 | 评价 |
|------|------|
| std::string 直接追加 | 无 prepend 能力，读时仍需中间拷贝 |
| 链表缓冲 | 零拷贝但分配碎片化、缓存不友好 |
| 环形缓冲 | prepend 天然 O(1)，但跨环边界（wrap）的线性读取/发送需要处理两段，readv 需要 iovec 数组 |
| 线性 + prependable 区（本实现） | 实现最简，覆盖全部实际场景（解析器几乎从不 prepend，保留能力即可） |

### 底层原理

`readv` 的 `iov[2]` 技巧：`iov[0]` 指向缓冲可写区，`iov[1]` 指向栈上 extrabuf，
一次调用收集两个区域。若 `iov[0]` 恰好填满则无需搬运；否则把 extrabuf 内容
append 到缓冲。这是"缓冲不足时零浪费"的标准做法（muduo 同款）。

---

## TcpConnection — 连接生命周期与保序发送

**职责**：连接的一切——Channel、读写缓冲、HttpContext、定时器、发送队列、
响应定序。**核心难点是并发**：worker 线程与 IO 线程共享连接的引用。

### 关键设计决策

- **`shared_ptr` + `enable_shared_from_this`**：worker/定时器/IO 回调各自持有
  引用计数，最后一个引用释放才析构。裸指针 + `delete this` 在并发下必然
  use-after-free（README 踩坑记录：ASan 实锤）。
- **延迟析构守卫**：`destroy()` 里 `queueInLoop([guard = shared_from_this()]{})`
  ——epoll_wait 返回的 events 数组可能仍持 Channel 指针，把 shared_ptr 压进
  pending queue，保证析构发生在 epoll for 循环之后。
- **`closed_` 原子防重入**：`handleClose` 被多路径调用（对端 FIN、EPOLLERR、
  发送失败、超时强关），`exchange(true)` 保证只执行一次完整清理。
- **发送快路径 + 粘滞 EPOLLOUT**（Step 16）：队列空闲时先直接 `send`
  （MSG_NOSIGNAL 防 SIGPIPE），小响应一次写完 → **0 次 epoll_ctl**；EAGAIN
  才入队 + `enableWriting()`。EPOLLOUT 注册后**永不注销**——ET 只在
  "不可写→可写"状态变化时触发，注册着不触发 = 零成本，下次腾空自动再触发
  （消除 epoll_ctl 乒乓，strace 实测 1000 请求仅 11 次 epoll_ctl）。
- **`markForClose` 先于投递评估**（Step 16 踩坑修复）：直接写成功路径没有
  EPOLLOUT 事件触发 `handleWrite`，`maybeCloseAfterSend` 是唯一评估点——
  标记必须在 `deliverResponse` **之前**设置，否则短连接永不关闭（实测挂到
  3s 心跳超时，fd 长时间占用）。
- **半关闭排空**：对端 `SHUT_WR`（FIN 但收侧仍开）→ `shutdown()` 停读 +
  markForClose，待发送队列排空后自动 close。直接 handleClose 会把在途 worker
  响应丢掉（实测客户端挂起到空闲超时）。
- **请求级并行 + seq 有序响应队列**（Step 16 核心）：同一连接的流水线请求
  各自分配 `seq = nextReqSeq_++` 独立提交 worker（乱序完成），响应到达
  `deliverResponse(seq, ...)`：`seq == nextSendSeq_` 直发并顺藤摸瓜排空
  `pendingResp_` 中连续段，否则暂存。发送队列入队顺序 = seq 递增顺序 →
  响应与请求一一对应保序（RFC 7230 §6.3.2）。`inflight_` 在途计数
  （≤ kMaxInflight_）有界：满则停止解析新请求（连接级背压）。

### 方案对比

| 方案 | 评价 |
|------|------|
| 每连接一把锁 + 共享状态 | 锁粒度粗、路径上处处加锁，7 万 tasks/s 的 worker 消费速率下锁竞争成为瓶颈 |
| 状态单线程拥有 + 跨线程投递（本实现） | 连接状态只在所属 IO 线程读写；worker 只读请求、构造响应，回投靠 `queueInLoop`——事件循环线程模型的红线被严格画在"投递"这一个操作上 |
| 无界在途（无限解析提交） | 内存无限膨胀；必须用 `inflight_` 上限 + 连接级停读形成背压闭环 |

### 底层原理

`sendfile` 与 SendItem 队列：文件响应 = `headers`（内存）+ `fd`（文件）两部分，
`handleWrite` 先发 headers 再 `sendfile` 文件体，二者可分别阻塞等待 EPOLLOUT。
FileFd 用 `shared_ptr<FileHandle>` 共享所有权——响应对象在
worker → queueInLoop lambda → pendingResp_ 暂存之间多次拷贝，最后一个副本
析构时恰好 close 一次（不提前关、不泄漏）。

---

## HttpContext — 请求解析状态机

**职责**：把 TCP 字节流解析成 `HttpRequest`，跨拆包累积状态，按 RFC 分类错误。

### 关键设计决策

- **显式状态机**：`kExpectRequestLine → kExpectHeaders → kExpectBody →
  KGotCompleteRequest`。状态累积在 context 内，多次 EPOLLIN 之间不丢失
  （单测按 1/3/7/64 字节逐块拆包验证）。
- **`parseRequest` 不消费不完整数据**：请求行/头部不完整时，返回 false 且
  **不推进 readIndex**——下一块到达时数据还在（拆包正确性根基）。
- **错误分类驱动响应码**：`kBadRequest→400`、`kMethodNotSupported→405`、
  `kVersionNotSupported→505`、`kHeaderTooLarge→413`、`kNotImplemented→501`、
  `kExpectationFailed→417`。分类在解析层完成，响应层（main.cpp）按分类映射，
  不重复判断报文细节。
- **协议安全加固**（Step 14）：Host 缺失/重复冲突 → 400（RFC 7230 §5.4）；
  CL+CL 冲突 → 400（走私向量）；**TE → 501 显式拒绝**（原实现静默忽略 TE，
  帧字节被当新请求解析 → 连锁 400，README 踩坑记录）；CL 全数字校验；
  header 8KB/行、64KB 累计 → 413；body 16MB 上限；`Expect: 100-continue`
  在头部解析完后、body 前同步回 100（每个请求至多一次），413 等拒绝先于
  状态转移检查，不会"先发 100 再反悔"。
- **URL 解码按 RFC 3986**：path 中 `+` 保持字面量（只对 query 做表单解码）；
  `%xx` 十六进制解码，非法编码原样保留。

### 方案对比

| 方案 | 评价 |
|------|------|
| 正则解析 | 直观但慢、错误分支难写、RFC 边界（重复头、大小写）难精确控制 |
| 逐字符状态机（本实现） | 每字符 O(1)，拆包/超限/错误分类全部显式可控；代价是代码量大（正是单测覆盖的重点） |
| 一次性读完再解析 | 无法处理长 body 与拆包；内存不可控 |

### 底层原理

跨拆包的正确性模型：`Buffer` 是**累积的**（readv 追加），`parseRequest` 消费
多少推进多少 readIndex。请求不完整时返回 false 不消费——"状态在 context 里，
字节在 buffer 里"，两个不变量互相独立，拼出确定性。

---

## HttpRequest / HttpResponse — 报文数据模型

**职责**：纯数据 + 序列化，不参与网络。

### 关键设计决策

- **Response 序列化汇合点**：`headersToString()` 是唯一"补全 Date 头"的地方
  （toString/sendResponse 都走它）——200/304/404/413/501/503 所有响应自动带
  Date（RFC 7231 §7.1.1.2 服务器 MUST），各 handler 无需逐个添加。
- **Date 头每秒缓存**（Step 17）：`httpDateNow()` 用 thread_local 缓存
  （秒值 + 格式化字符串），秒边界首次访问才 `gmtime_r + strftime`，同秒内
  直接复用——14 万 req/s 下每请求省两次系统调用 + locale 处理。线程本地
  而非全局：响应序列化只发生在 IO 线程，无锁无伪共享；跨线程（理论上任何
  线程都可能调用）也各自缓存，正确性不依赖调用线程分布。
- **HEAD 只发头部**：`toString(includeBody=false)`，但 Content-Length 保留
  真实长度（RFC 7231 §4.3.2）——HEAD 的语义是"和 GET 一样的头，没有体"。
- **`makeError` 工厂**：统一错误响应模板（HTML body + Content-Type +
  Connection: close 默认值），503 场景会覆盖 closeConnection_（保持长连接）。
- **FileFd 所有权**：见 TcpConnection 节——响应携带 fd 的所有权而不是路径，
  发送层拿到即用，杜绝二次 open。

### 方案对比

| 方案 | 评价 |
|------|------|
| handler 各自拼字符串 | Date 头漏加/重复、转义不一致；响应结构散落 |
| 对象 + 统一序列化（本实现） | 单一出口，协议细节（Date、Connection: close、CL）集中维护 |

---

## Router — 路由

**职责**：method + path → handler 的查找表。

### 关键设计决策

- **三种模式**：精确匹配（`/`、`/stats`）、参数化（`/user/:id` → params
  `{"id": "42"}`）、通配符（`/static/*` 前缀匹配）。URL 解码后匹配
  （`/user/%3Aid` 这类编码攻击不会误命中参数模式——Step 14 修复：
  参数模式跳过精确查找，`pathToMethods_` 查不到就 404）。
- **`std::map<RouteKey, Handler>` 排序存储**：method + path 为 key 的排序树，
  查找 O(log n)；route 数量级小（几十条），排序结构让"精确匹配优先于参数
  匹配优先于通配符"的优先级判定变得直接。
- **handler 签名携带 params**：参数解析结果以 `std::map<std::string,
  std::string>` 传出，handler 无状态、可测试（单测 12 例覆盖三种模式与
  优先级）。

### 方案对比

| 方案 | 评价 |
|------|------|
| 正则表 | 最强表达力（README 已知局限：不支持正则），但编译期构造 + 运行时开销大，学习代价高 |
| 字典树（前缀树） | 大规模路由（千级）最优；本项目路由量级小，map 已够 |
| map 三分层（本实现） | 代码直观、测试充分，扩展正则只需加一层 |

---

## StaticFileHandler — 静态文件服务

**职责**：磁盘文件 → HTTP 响应，含安全防护与缓存。

### 关键设计决策

- **realpath 前缀校验 → openat2 原子化**（Step 16 演进）：realpath 只消除
  "已有"符号链接，校验与 open 之间仍有替换窗口（TOCTOU）。`openat2 +
  RESOLVE_NO_SYMLINKS` 在 open 时刻由内核原子校验全路径分量；老内核
  （< 5.6）回退 `O_NOFOLLOW` + `fstat` dev/ino 比对（残余窗口仅限中间分量，
  代码注释说明）。**文件 fd 一次打开直传发送层**——不再按路径二次 open，
  彻底关闭窗口。
- **大小分叉**：`> 64KB` sendfile 零拷贝（内核态 page cache → DMA）；
  `≤ 64KB` LRU 内容缓存（省 open/read syscall，实测静态小文件 74.5k req/s）。
- **304 协商缓存**：`If-Modified-Since` 与 mtime 比较，未变 → 304 + 空 body
  （Last-Modified 用 `httpDate(mtime)` 格式化，与响应 Date 同格式）。
- **MIME 映射 + 目录 index.html**：扩展名 → Content-Type 表；`/` 结尾映射
  index.html，404 时回退路由层统一处理。

### 方案对比

| 方案 | 评价 |
|------|------|
| 字符串黑名单禁 `..` | `//`、`%2e%2e`、符号链接全可绕过（README 踩坑记录） |
| realpath 前缀比较（旧） | 消除已有符号链接，但校验与 open 之间的替换窗口仍在 |
| openat2 原子校验（现） | 窗口关闭到"一次系统调用"；代价是内核版本依赖（回退路径保底） |
| 每请求 open/read | 静态文件高频场景 syscall 开销大；LRU 缓存 ≤64KB 文件命中即省掉全部 |

### 底层原理

`sendfile(fd_out, fd_in, &offset, count)` 把文件内容从 page cache 直接送入
socket，全程不经过用户态。零拷贝的三个前提：发送端 fd 可写、文件 fd 可读、
count 不为 0（0 字节文件 sendfile 返回 0，会误入错误分支——代码里显式跳过，
见 `TcpConnection::handleWrite`）。

---

## ThreadPool — 工作线程池

**职责**：执行路由/静态文件等 CPU 型业务任务，与 IO 线程解耦。

### 关键设计决策

- **one-loop-per-thread 重构**（Step 16，本版本最重要的性能改动）：旧实现是
  "单共享队列 + mutex/condvar"，提交与消费共享一把锁，且 `notify_one` 有
  空唤醒。重构后 N 个 worker 各自运行独立 `EventLoop`，提交路径只剩：
  ① `inFlight_.fetch_add`（有界判据，返回值为准，拒绝时 fetch_sub 回滚）；
  ② `next_.fetch_add % N`（RR 分发，无锁）。**无共享队列、无锁竞争、无
  condvar**。实测 -c1000 动态路由 101.2k → 140.4k（+39%）。
- **有界 = 排队 + 执行中**：`inFlight_` 在提交时 fetch_add、任务执行完
  fetch_sub。比"仅队列长度"略紧，默认 1024 在满负载下触发 503 背压
  （README 性能节：c=2000/5000 时约 0.5% 503，设计内行为）。
- **任务异常隔离**（Step 17）：worker 包裹层 try/catch——业务 handler 的
  异常已被 main.cpp 包装层转成 500，这里兜底保证任何逃逸异常（含包装层自身
  bug）都不杀死 worker 线程，且 `inFlight_` **无条件自减**：漏减会让在途
  计数虚高 → 误判背压（多余 503）+ 优雅关闭 drainCheck 永不收敛（挂满 10s）。
- **stop 语义**：`stop()` 先 quit 所有 loop（eventfd 唤醒，避免 join 卡在
  5s epoll_wait 超时），join 后 `inFlight_ = 0`——join 期间执行中的任务已
  自减完毕，剩余值 = 被丢弃的排队任务数。**stop 后仍受理投递**
  （loops_ 存活到析构）：对应 deadline 兜底时 IO 线程继续 tryRun 的场景，
  投递落进存活队列（永不执行），不变式保持、析构安全。

### 方案对比

| 方案 | 评价 |
|------|------|
| 单共享队列 + mutex/condvar（旧） | 提交路径锁竞争 + notify_one 空唤醒；-c1000 101.2k req/s 到顶 |
| 每任务新建线程 | 线程创建/销毁成本 + 上下文切换风暴 |
| N 个独立 loop + RR 分发（现） | 提交路径两个原子操作；worker 数少（2~3）时各 loop 独立消费，天然分摊 |
| IO 线程直执行（快路径） | README 列为下一步方向：队列空时省掉跨线程 handoff；代价是 IO 线程被长任务阻塞 |

### 底层原理

提交路径的原子性：`fetch_add` 的返回值是**单调唯一**的——并发提交者各拿各的
序号，既当在途计数（有界判据）又当 RR 分发下标（mod N），一个原子操作干两件事，
且杜绝"先 load 再 add"的检查-受理间隙双重放行。

---

## TimerQueue — 定时器

**职责**：timerfd + 最小堆管理定时任务（连接空闲超时、排空检查、心跳扫描）。

### 关键设计决策

- **timerfd 集成 epoll**：定时器是 fd，天然并入事件循环——无需单独线程
  轮询，无唤醒竞态。
- **自实现最小堆**（Step 14 重构）：`timerHeap_` 按到期时间升序 + `id2index_`
  维护 id → 堆下标。cancel 从 O(n) 线性扫描降到 O(log n)（Step 14 踩坑：
  0-based vector 混用 1-based 堆下标公式，`(i-1)/2` / `2i+1`，修出 core）。
- **惰性重置**（Step 16）：连接空闲超时不再每请求 cancel + addTimer
  （每请求一次 timerfd_settime），改为连接只记 `lastActiveTime_`（一个
  relaxed atomic，**零 syscall**），1s 心跳定时器扫描所有连接，超时才强关。
  timerfd_settime 从每请求 1 次 → 启动时 1 次（strace 实测：1000 请求 5 次
  vs 旧实现 1000 次）；代价是超时精度从精确毫秒降为 ±1s。
- **`addTimer`/`cancel` 断言 IO 线程**（Step 11）：定时器全生命周期单线程，
  零锁竞争；断言把误用变成崩溃而非静默竞态。

### 方案对比

| 方案 | 评价 |
|------|------|
| 时间轮 | O(1) 触发，适合海量短定时器；本项目定时器数量级（连接数）下最小堆已够，且支持任意到期时间 |
| std::multimap<expiry, timer>（旧） | 同微秒 key 覆盖丢定时器（Step 10 修复）；cancel O(n) |
| 最小堆 + id2index（现） | cancel O(log n)；到期时间允许重复（堆下标不唯一） |

### 底层原理

`timerfd_create(CLOCK_MONOTONIC)` + `timerfd_settime(ITIMER_REAL)`：到期一次
产生可读事件，内核自动置读端计数；重复定时器靠 interval 参数，一次 settime
周期触发。最小堆堆化在 addTimer/cancel 时执行，`earliestChanged` 决定是否
需要重置 timerfd（只有堆顶变化才动内核）。

---

## SignalHandler — 信号处理与优雅关闭

**职责**：把 POSIX 信号转成 eventfd 事件，触发排空式优雅关闭。

### 关键设计决策

- **信号 → eventfd → epoll**：信号处理函数里只写 eventfd（异步信号安全），
  epoll 唤醒主循环执行关闭逻辑——信号处理不碰任何非 async-signal-safe 的
  API（mutex/堆分配在信号上下文全禁止）。
- **排空状态机**（Step 15 踩坑修复）：SIGTERM → 停 accept → 每 100ms 检查
  活跃连接数与在途任务，归零或 10s 兜底则退出。排空期请求**照常处理**，
  响应强制 `Connection: close`——静默丢弃会让客户端挂起（实测 3s）；
  `markForClose` 在发送队列清空后自动关连接，排空自然收敛。
- **10s 兜底必须短路**：`deadline` 到了无论还有没有在途任务都要退出
  （否则 `&` 绑定下 deadline 退化为"再等 100ms"，永远挂住——README 踩坑记录）。

### 方案对比

| 方案 | 评价 |
|------|------|
| 信号处理函数里直接做关闭 | 不可重入、非 async-signal-safe；关闭动作可能跨越信号上下文 |
| self-pipe 技巧 | 经典做法；eventfd 是它的现代替代（语义更简单，8 字节计数，无 SIGPIPE 问题） |
| 信号 → eventfd（本实现） | 信号面最小化，关闭逻辑完全在主循环内单线程执行 |

---

## Metrics — 指标

**职责**：6 个 counter + `/stats` JSON。

### 关键设计决策

- **全 atomic，零锁**：`fetch_add` 在 x86 是单条 `LOCK INC`，多线程并发写
  不需要锁；读 `/stats` 也不等锁（宽松一致性对展示性指标完全够用）。
- **6 个 counter**：totalRequests / activeConnections / errors4xx /
  errors5xx / bytesReceived / bytesSent。活跃连接数同时是优雅关闭的收敛
  判据（排空检查依赖它）。

### 方案对比

| 方案 | 评价 |
|------|------|
| mutex 保护 | 读多写多路径锁开销大，且锁在热路径上 |
| atomic（本实现） | 单指令，无等待；代价是丢失"精确快照"（多 counter 间无一致视图）——对监控可接受 |

---

## TcpServer — 服务器组装

**职责**：把 Acceptor + IO 子循环 + 连接管理 + 回调装配起来。

### 关键设计决策

- **`connections_` 集合 + mutex**：所有活跃连接的 shared_ptr 由 server 持有
  （防止连接被 IO 线程释放后 server 还在遍历）；`forEachConnection` 用于心跳
  扫描。集合访问跨线程（accept 回调在主线程、关闭回调在 IO 线程）→ mutex。
- **关闭动作投递到所属线程**（Step 15/16 修复）：`shutdown()` 时
  `conn->getLoop()->queueInLoop([conn]{ conn->shutdown(); })`——直接跨线程调
   `conn->shutdown()` 会与 IO 线程并发改 Channel/发送队列（实测 SIGSEGV：
  connections_ 红黑树遍历崩溃 + destroy() 抛 bad_weak_ptr）。投递到所属线程
  执行，消灭跨线程触碰。
- **拆除顺序固定**（两个 ASAN 实测崩溃都出在顺序上）：先 `threadPool.stop()`
  （worker 任务里的 `queueInLoop` 还引用子循环，子循环此刻必须活着）→
  `server.shutdown()`（子循环 quit+join、连接全关）→ 函数返回后才释放
  loops_（此时所有投递方已死）。

---

## 跨模块设计：异常隔离

**问题**：任何单请求/单连接的异常都可能导致整个进程 `std::terminate`——事件
循环线程栈上逃逸未捕获异常 = 进程死亡。对"以生产工程标准要求自己"的服务，
这是不可接受的故障模式。

**三层防线**（Step 17）：

```
业务层 (main.cpp worker lambda)   handler 异常 → LOG_ERROR + 500 响应
        ↓ 逃逸
任务层 (ThreadPool 包裹)           catch → LOG_ERROR，inFlight_ 无条件自减
        ↓ 逃逸（IO 线程回调、定时器）
事件层 (EventLoop)                 catch → LOG_ERROR，循环继续
```

| 层 | 捕获异常后 | 不做的后果 |
|----|-----------|-----------|
| 业务层 | 客户端收到显式 500，不挂起；errors5xx++ 可观测 | 客户端等超时，连接被心跳误杀 |
| 任务层 | worker 线程存活；在途计数平衡 | worker 线程死亡（进程崩溃）；在途泄漏 → 误 503 + 排空挂 10s |
| 事件层 | IO 线程存活，其余连接不受影响 | 全部连接同时断开 |

**为什么不是 try/catch 包住整个 loop()？** 那样丢的就不仅仅是"出错的回调"，
`callingPendingFunctors_` 等循环内状态会残留在异常展开后的未知位置；catch 在
每个回调粒度上，状态机每步都是完整的。

**为什么不 catch 所有异常（`catch(...)`）**？`std::exception` 之外还有
`catch(...)` 兜底——两者都有，前者可带 `e.what()` 日志，后者处理未知异常。

---

## 跨模块设计：背压与保序

**问题**：请求进入系统有三道闸门——连接级（kMaxInflight_）、线程池级
（inFlight_ ≤ max_queue_size）、TCP 级（socket 缓冲区）。任一道失守都会
内存失控或响应乱序。

**两道有界闸门 + 一个序**：

| 机制 | 位置 | 行为 |
|------|------|------|
| 连接级在途上限 | `processBufferedRequests` 循环条件 | 在途 ≥ kMaxInflight_ 停止解析新请求（连接读侧背压，天然流动控制） |
| 线程池在途上限 | `ThreadPool::tryRun` | fetch_add 超过上限 → 503，**不关连接**（拒绝→重连→更忙风暴 + TIME_WAIT 堆积，README 踩坑记录） |
| seq 有序响应 | `deliverResponse` | 乱序完成先暂存 pendingResp_，轮到 seq 才发送（RFC 7230 §6.3.2） |

**为什么 503 不关连接**？关连接版：客户端"拒绝→重连→更忙"自放大 + 服务端
作为主动关闭方堆积 TIME_WAIT（实测 3000+，占满 fd）。不关连接：拒绝成本
恒定，连接留着继续服务后续请求。

**为什么要有 seq 定序**？多 worker 乱序完成 + queueInLoop 入队竞态曾导致
`[200,400] → [400,400]` 响应错配（README 踩坑记录第一条）。分配 seq 后，
发送队列入队顺序 = seq 递增顺序，错误响应也按 seq 定序（400/503 都是
"本请求 seq"的响应）。

---

## 跨模块设计：syscall 削减

**原则**：用户态代码是廉价的，系统调用与内核切换是昂贵的。每个请求省一次
syscall，14 万 req/s 下就是每秒 14 万次切换。

| 削减点 | 做法 | 实测效果 |
|--------|------|----------|
| 唤醒去重 | `wakeupPending_` 原子：只有第一个投递者写 eventfd | eventfd 写入从每投递 1 次 → 每轮唤醒 1 次 |
| EPOLLOUT 乒乓 | send 先直接写，EAGAIN 才 `enableWriting()`；注册后不注销 | epoll_ctl 从每请求 2 次 → 1000 请求共 11 次 |
| timerfd 惰性重置 | 1s 心跳扫描替代每请求 cancel/addTimer | timerfd_settime 从每请求 1 次 → 1000 请求共 5 次 |
| Date 头缓存 | 每秒缓存格式化结果 | `gmtime_r + strftime` 从每请求 2 次 → 每线程每秒 1 次 |
| 静态文件缓存 | LRU 内存缓存 ≤64KB 文件 | open/read 从每请求 2 次 → 命中 0 次 |

strace 基线：1000 个 keep-alive 请求，削减前每请求 7 次系统调用
（readv / send / epoll_ctl×2 / timerfd_settime / eventfd×2），削减后 ≈4 次；
c=100 动态路由 48.5k→52.9k（+9%）、静态小文件 67.4k→80.4k（+19%）。

---

## 参考资料

- [muduo — 陈硕的 C++ 网络库](https://github.com/chenshuo/muduo)（主从 Reactor、
  Buffer 三区模型、queueInLoop 延迟析构、定时器惰性重置的思路来源）
- [The C10K Problem](http://www.kegel.com/c10k.html)
- Linux man: `epoll(7)`, `timerfd_create(2)`, `eventfd(2)`, `readv(2)`,
  `openat2(2)`, `realpath(3)`, `sendfile(2)`
- RFC 7230（HTTP/1.1 消息语义与路由）、RFC 7231（方法/状态码）、RFC 3986（URI）
