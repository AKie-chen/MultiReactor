#!/usr/bin/env python3
"""用 PMU 的 cycles 测「每请求 CPU 成本」，并对账三种口径——理论上限的分子。

**为什么必须按 CPU 统计（BENCH_PMU_CPUS）**：本机实测 `perf stat -p <pid>` 会
严重扰动服务端（-c500：252k → 186k req/s，-27%；两口径差 23%）。per-task 事件在
高频跨核迁移的进程上，每次迁移都要 IPI 重新编程计数器。改用 `-C` + taskset 把
服务端/压测端分到不同核后，扰动降到 -2.6%，且计数器只看到服务端。

**三种口径对不上的原因（实测，很重要）**：以服务端 pin 在 CPU 0-3、压测端在
4-7 为例，-c500 动态路由稳态下同一窗口内：

    PMU cycles（0-3 全部非停机周期）  3.56 核
    /proc/stat 忙（user+sys+irq+soft） 2.96 核   ← 其中 irq 0.20 + softirq 0.89
    服务端进程 /proc/<pid>/stat        2.18 核   ← cpu_measure.py 用的就是这个

差的 0.78 核是 softirq/irq 上下文里的工作，**不计入服务端进程**：服务端 write()
把响应写进 loopback 后，对端（压测端）的 TCP 收包处理是在服务端所在 CPU 的软中断
上下文里跑的。所以：
  - 任务口径（/proc）**低估**：漏掉自己流量引发的软中断开销 → 上限偏乐观；
  - 机器口径（PMU）**高估**：把同机压测端的收包也算了进来 → 上限偏悲观；
  - 真实部署（压测端在另一台机器）落在两者之间。

本机 vPMU 只虚拟化了 cycles（instructions / cache-misses 恒为 0），IPC/缓存分析
做不了；频率用自旋标定值（8 线程自旋 5s：cycles/墙钟 = 3.190 GHz，与标称 3.194
一致，无降频）。

用法:
  taskset -c 0-3 ./build/multireactor --port 8081 -i 3 -w 1 &
  BENCH_PORT=8081 BENCH_SERVER_PID=<pid> BENCH_PMU_CPUS=0-3 \
    python3 bench/pmu_measure.py taskset -c 4-7 wrk -t4 -c500 -d10s http://127.0.0.1:8081/user/123
环境变量: BENCH_PMU_CPUS(如 0-3) BENCH_PMU_FREQ BENCH_PMU_BASELINE(空载基线秒,默认2)
          BENCH_PMU_SYSCALLS=1(同时统计每请求 syscall 数)
"""
import json, os, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpu_measure import HZ, NCPU, proc_ticks, server_pid, stats_requests

FREQ = float(os.environ.get("BENCH_PMU_FREQ", "3.190e9"))
CPUS = os.environ.get("BENCH_PMU_CPUS")
BASELINE = float(os.environ.get("BENCH_PMU_BASELINE", "2"))
EVENTS = ["cycles"]
if os.environ.get("BENCH_PMU_SYSCALLS"):
    EVENTS += ["syscalls:sys_enter_" + s for s in
               ("epoll_wait", "epoll_ctl", "readv", "recvfrom", "sendto", "write",
                "futex", "accept4")]

# /proc/stat 的 cpu 行：user nice system idle iowait irq softirq steal ...
BUSY_IDX = [0, 1, 2, 5, 6]          # user nice system irq softirq


def cpu_ids(spec):
    out = []
    for part in spec.split(","):
        a, _, b = part.partition("-")
        out += list(range(int(a), int(b or a) + 1))
    return out


def per_cpu(cpus):
    """选中 CPU 的 /proc/stat 累计值，返回 (忙, 其中 irq+softirq, idle)。"""
    busy = irqsoft = idle = 0
    want = set(cpus)
    for line in open("/proc/stat"):
        f = line.split()
        if not f[0].startswith("cpu") or f[0] == "cpu" or not f[0][3:].isdigit():
            continue
        if int(f[0][3:]) not in want:
            continue
        v = [int(x) for x in f[1:]]
        busy += sum(v[i] for i in BUSY_IDX)
        irqsoft += v[5] + v[6]
        idle += v[3] + v[4]
    return busy, irqsoft, idle


def perf_count(scope, cmd):
    fd, path = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    argv = ["perf", "stat", "-j", "-e", ",".join(EVENTS), "-o", path] + scope + ["--"] + cmd
    t0 = time.monotonic()
    try:
        subprocess.run(argv)
    finally:
        dt = time.monotonic() - t0
    counts = dict.fromkeys(EVENTS, 0.0)
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            ev = json.loads(line)
            try:
                counts[ev["event"]] = float(ev["counter-value"])
            except ValueError:
                pass                        # "<not counted>"
    os.unlink(path)
    return counts, dt


def main():
    cmd = sys.argv[1:]
    if not cmd:
        sys.exit(__doc__)
    pid = server_pid()
    if CPUS == "all":                       # 整机：用于核对 cpu_measure.py 的整机口径
        scope, cpus = ["-a"], list(range(NCPU))
    elif CPUS:
        scope, cpus = ["-C", CPUS], cpu_ids(CPUS)
    else:
        scope, cpus = ["-p", str(pid)], None

    base_cyc = 0.0
    if CPUS and BASELINE > 0:               # 空载基线：扣除共租户（VSCode/claude）
        base_cyc = perf_count(scope, ["sleep", str(BASELINE)])[0]["cycles"]

    b0, i0, _ = per_cpu(cpus) if cpus else (0, 0, 0)
    p0, r0 = proc_ticks(pid), stats_requests()
    c, dt = perf_count(scope, cmd)
    p1, r1 = proc_ticks(pid), stats_requests()
    b1, i1, _ = per_cpu(cpus) if cpus else (0, 0, 0)

    n = r1 - r0
    cyc = c["cycles"] - base_cyc
    if n <= 0 or cyc <= 0:
        sys.exit(f"窗口内没有可用样本：请求={n} cycles={cyc:.0f}（服务端在压测期间几乎没运行？）")

    us_machine = cyc / n / FREQ * 1e6                    # 机器口径：CPU 上全部周期
    us_task = (p1 - p0) / HZ / n * 1e6                   # 任务口径：/proc 进程
    mode = "整机 -a" if CPUS == "all" else (f"按CPU {CPUS}" if CPUS else "按进程(会扰动，仅供参考)")
    print(f"\n[PMU] 模式={mode}  时长={dt:.2f}s 请求={n} 吞吐={n/dt:,.0f} req/s")
    print(f"[PMU] 每请求成本：机器口径 {us_machine:.2f} µs（{cyc/n:,.0f} cycles）"
          f"   任务口径 {us_task:.2f} µs（/proc）")
    print(f"[PMU] 服务端占用：机器口径 {cyc/FREQ/dt:.2f} 核  |  任务口径 {(p1-p0)/HZ/dt:.2f} 核"
          f"  = 整机的 {cyc/FREQ/dt/NCPU*100:.0f}% / {(p1-p0)/HZ/dt/NCPU*100:.0f}%")
    if cpus:                                # 三方对账：差值是压测端 / 共租户 / 其他上下文
        label = "整机" if CPUS == "all" else "CPU" + CPUS
        rest = "（压测端 + 其他上下文）" if CPUS == "all" else "（共租户 / 其他上下文）"
        print(f"[PMU] {label} 时间账：忙 {(b1-b0)/HZ/dt:.2f} 核（其中 irq+softirq "
              f"{(i1-i0)/HZ/dt:.2f} 核，实测就地处理不计入 ksoftirqd）| 服务端进程 "
              f"{(p1-p0)/HZ/dt:.2f} 核 → 差 {(b1-b0)/HZ/dt-(p1-p0)/HZ/dt:.2f} 核 {rest}")
    for ev in EVENTS:
        if ev.startswith("syscalls:"):
            print(f"[PMU]   {ev.split('sys_enter_')[1]:<14} = {c[ev]/n:.3f}/请求")
    print(f"[PMU] CPU 界：机器口径（{NCPU}×{FREQ/1e9:.3f}GHz ÷ {cyc/n:,.0f}）≈ {NCPU*FREQ/(cyc/n):,.0f} req/s"
          f"  任务口径 ≈ {NCPU/us_task*1e6:,.0f} req/s"
          f"  实测 {n/dt:,.0f}")


if __name__ == "__main__":
    main()
