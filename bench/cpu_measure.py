#!/usr/bin/env python3
"""测「服务端每请求 CPU 成本」与整机占用——理论上限就是这么算出来的。

跑一条压测命令，同时统计：
  - 服务端进程的 CPU（/proc/<pid>/stat 的 utime+stime，多线程进程是聚合值）
  - 整机 CPU（/proc/stat  cpu  行的忙字段）
  - 服务端自己的请求计数（/stats）
据此给出每请求 µs 与"服务端独占整机时的理论上限 = 核数 / 每请求成本"。

因为压测端与服务端同机，闭环数字通常由两边合计的 CPU 决定，所以脚本把
"压测端 ≈ 整机 - 服务端" 也打印出来；两者相加除以核数就是这台机器的饱和度。

用法: python3 bench/cpu_measure.py <压测命令...>
      BENCH_SERVER_PID=<pid> python3 bench/cpu_measure.py ...   # 指定服务端进程
示例: python3 bench/cpu_measure.py wrk -t8 -c500 -d10s http://127.0.0.1:8080/user/123
      python3 bench/cpu_measure.py python3 bench/pipeline_bench.py 16 256 5
"""
import json, os, subprocess, sys, time, urllib.request

HOST = os.environ.get("BENCH_HOST", "127.0.0.1")
PORT = int(os.environ.get("BENCH_PORT", "8080"))
HZ = os.sysconf("SC_CLK_TCK")
NCPU = os.cpu_count()


def server_pid():
    pid = os.environ.get("BENCH_SERVER_PID")
    if pid:
        return int(pid)
    out = subprocess.run(["pgrep", "-x", "multireactor"], capture_output=True, text=True).stdout.split()
    if not out:
        sys.exit("找不到服务端进程（pgrep -x multireactor）；用 BENCH_SERVER_PID 指定")
    return int(out[0])


def proc_ticks(pid):
    with open(f"/proc/{pid}/stat") as f:
        b = f.read().rsplit(") ", 1)[1].split()
    return int(b[11]) + int(b[12])          # utime + stime（字段 14/15）


def sys_ticks():
    with open("/proc/stat") as f:
        f = f.readline().split()[1:]
    v = [int(x) for x in f]
    return sum(v) - v[3] - v[4]             # 忙 = 总 - idle - iowait


def stats_requests():
    with urllib.request.urlopen(f"http://{HOST}:{PORT}/stats", timeout=5) as r:
        return json.load(r)["requests"]


def main():
    cmd = sys.argv[1:]
    if not cmd:
        sys.exit(__doc__)
    pid = server_pid()
    p0, s0, r0 = proc_ticks(pid), sys_ticks(), stats_requests()
    t0 = time.monotonic()
    subprocess.run(cmd)
    dt = time.monotonic() - t0
    p1, s1, r1 = proc_ticks(pid), sys_ticks(), stats_requests()

    n = r1 - r0
    srv = (p1 - p0) / HZ / dt
    sysc = (s1 - s0) / HZ / dt
    us = (p1 - p0) / HZ / n * 1e6
    print(f"\n[CPU] 时长={dt:.2f}s 请求={n} 吞吐={n/dt:,.0f} req/s")
    print(f"[CPU] 服务端={srv:.2f} 核  压测端≈{sysc-srv:.2f} 核  整机={sysc:.2f}/{NCPU} 核"
          f"（{sysc/NCPU*100:.0f}%）")
    peer_us = (sysc - srv) * dt / n * 1e6      # 压测端每请求 µs（同机共置时与压测端争核）
    ceiling_alone = NCPU / us * 1e6
    ceiling_co = NCPU / (us + peer_us) * 1e6
    print(f"[CPU] 每请求成本：服务端 {us:.2f}µs  压测端 {peer_us:.2f}µs（含内核时间）")
    print(f"[CPU] 服务端独占 {NCPU} 核的上限 ≈ {ceiling_alone:,.0f} req/s"
          f"；同机共置上限 ≈ {ceiling_co:,.0f} req/s"
          f"（实测 {n/dt:,.0f}，达成率 {n/dt/ceiling_co*100:.0f}%）")


if __name__ == "__main__":
    main()
