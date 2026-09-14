#!/usr/bin/env python3
"""单连接/多连接 HTTP 流水线基准（数据见 bench/README.md 第 3、4 节）。

批量发送 K 个请求（一次性 sendall），再收齐 K 个响应并校验：
  - 每个响应都是 200
  - 响应体与请求序号一一对应（验证服务端 seq 保序重排，字节级比对）
  - 统计 503 出现情况（线程池背压）

id 用 4 位定长（1000..）→ 所有响应等长，热路径按步长切片校验；
末了用服务端 /stats 的请求计数交叉验证客户端计数，防止客户端自身算错。

多进程：procs>1 时按连接 fork 子进程（各自独立 GIL）。Python 单进程每请求约
7µs CPU，单进程跑不满高核数服务端——4 vCPU 上 procs=1 够用，核数上去后 procs
要跟着涨。起止用绝对 deadline 对齐，子进程结果经管道回传后在父进程聚合；
父进程只负责发令与统计，不计入客户端 CPU。

用法: pipeline_bench.py <conns> <depth> <duration_s> [procs]
      BENCH_HOST=10.0.0.5 BENCH_PORT=8080 python3 bench/pipeline_bench.py 16 256 10 4
示例: python3 bench/pipeline_bench.py 16 256 5      # 单进程
      python3 bench/pipeline_bench.py 32 128 10 4   # 4 进程
"""
import socket, sys, time, json, os, threading, traceback, urllib.request

HOST = os.environ.get("BENCH_HOST", "127.0.0.1")
PORT = int(os.environ.get("BENCH_PORT", "8080"))
ID_BASE = 1000  # 4 位定长 id，保证响应等长


def stats_requests():
    with urllib.request.urlopen(f"http://{HOST}:{PORT}/stats", timeout=5) as r:
        return json.load(r)["requests"]


def read_response(sock, buf):
    """从 buf 中解析一个完整响应并就地消费，返回 (head, body, head_end)。"""
    while b"\r\n\r\n" not in buf:
        buf += sock.recv(4096)
    head_end = buf.find(b"\r\n\r\n") + 4
    clen = 0
    for line in bytes(buf[:head_end]).split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":")[1])
    while len(buf) < head_end + clen:
        buf += sock.recv(4096)
    head, body = bytes(buf[:head_end]), bytes(buf[head_end:head_end + clen])
    del buf[:head_end + clen]
    return head, body, head_end


def probe_stride():
    """探测响应固定长度与 body 偏移；三个不同 id 的响应长度必须一致。"""
    s = socket.create_connection((HOST, PORT))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    buf = bytearray()
    strides, offs = set(), set()
    for pid in (ID_BASE, ID_BASE + 1, ID_BASE + 255):
        s.sendall(f"GET /user/{pid} HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n".encode())
        head, body, head_end = read_response(s, buf)
        assert head.startswith(b"HTTP/1.1 200 OK"), head[:40]
        assert body == f"user id: {pid}".encode(), body
        strides.add(head_end + len(body))
        offs.add(head_end)
    s.close()
    assert len(strides) == 1 and len(offs) == 1, f"响应不等长: {strides},{offs}"
    return strides.pop(), offs.pop()


def slow_parse(buf, statuses):
    """慢路径：逐条解析统计状态码（缓存的 5xx 会破坏定长假设，需回退到此路径）。"""
    while b"\r\n\r\n" in buf:
        head_end = buf.find(b"\r\n\r\n") + 4
        clen = 0
        for line in bytes(buf[:head_end]).split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                clen = int(line.split(b":")[1])
        if len(buf) < head_end + clen:
            break
        statuses[buf[9:12]] = statuses.get(buf[9:12], 0) + 1
        del buf[:head_end + clen]
    return buf


def run_conn(depth, deadline, stride, body_off, results, idx):
    ids = [ID_BASE + i for i in range(depth)]
    payload = b"".join(f"GET /user/{i} HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n".encode() for i in ids)
    exp_bodies = [f"user id: {i}".encode() for i in ids]
    need = depth * stride
    n_req = n_bad = n_503 = 0
    lat = []
    slow = False
    s = socket.create_connection((HOST, PORT))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(5)
    try:
        while time.monotonic() < deadline:
            t0 = time.monotonic()
            s.sendall(payload)
            buf = bytearray()
            while len(buf) < need:
                chunk = s.recv(262144)
                if not chunk:
                    raise ConnectionError("server closed")
                buf += chunk
                if not slow and b"503 Service" in buf:
                    slow = True
            lat.append(time.monotonic() - t0)
            if not slow:
                for j in range(depth):
                    off = j * stride
                    if buf[off:off + 15] != b"HTTP/1.1 200 OK" or \
                       buf[off + body_off:off + stride] != exp_bodies[j]:
                        n_bad += 1
                n_req += depth
            else:
                statuses = {}
                slow_parse(buf, statuses)
                n_req += statuses.get(b"200", 0)
                n_503 += statuses.get(b"503", 0)
                slow = False  # 下一轮重新对齐快路径
    except (socket.timeout, ConnectionError, OSError) as e:
        results[idx] = {"err": f"{type(e).__name__}:{e}", "req": n_req, "503": n_503, "bad": n_bad, "lat": lat}
        s.close(); return
    s.close()
    results[idx] = {"req": n_req, "503": n_503, "bad": n_bad, "lat": lat}


def run_conns(conns, depth, deadline, stride, body_off):
    """跑 conns 条连接直到 deadline，返回该组连接的聚合结果。"""
    results = [None] * conns
    threads = [threading.Thread(target=run_conn, args=(depth, deadline, stride, body_off, results, i))
               for i in range(conns)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return {
        "req": sum(r["req"] for r in results),
        "bad": sum(r["bad"] for r in results),
        "n503": sum(r["503"] for r in results),
        "lat": [x for r in results for x in r["lat"]],
        "errs": [r["err"] for r in results if "err" in r],
    }


def write_all(fd, data):
    mv = memoryview(data)
    while mv:
        mv = mv[os.write(fd, mv):]


def fork_child(conns, depth, deadline, stride, body_off):
    """fork 一个子进程跑 conns 条连接，返回 (pid, 读端 fd)。"""
    rfd, wfd = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(rfd)
        try:
            blob = json.dumps(run_conns(conns, depth, deadline, stride, body_off)).encode()
        except BaseException:
            blob = json.dumps({"fatal": traceback.format_exc()}).encode()
        try:
            write_all(wfd, blob)
        finally:
            os._exit(0)
    os.close(wfd)
    return pid, rfd


def read_all(fd):
    chunks = []
    while True:
        b = os.read(fd, 1 << 20)
        if not b:
            break
        chunks.append(b)
    os.close(fd)
    return b"".join(chunks)


def bench(conns, depth, duration, procs=1):
    procs = max(1, min(procs, conns))
    if procs > (os.cpu_count() or 1):
        print(f"注意: procs={procs} > 可用核 {os.cpu_count()}，压测端自身会互相抢核", file=sys.stderr)
    stride, body_off = probe_stride()
    r0 = stats_requests()
    t0 = time.monotonic()
    deadline = t0 + duration
    parts = []
    if procs == 1:
        parts.append(run_conns(conns, depth, deadline, stride, body_off))
    else:
        kids = []
        for i in range(procs):
            n = conns // procs + (1 if i < conns % procs else 0)
            kids.append(fork_child(n, depth, deadline, stride, body_off))
        for pid, rfd in kids:
            blob = read_all(rfd)
            os.waitpid(pid, 0)
            if not blob:
                print("子进程异常退出（无输出）", file=sys.stderr)
                sys.exit(1)
            part = json.loads(blob)
            if "fatal" in part:
                print(part["fatal"], file=sys.stderr)
                sys.exit(1)
            parts.append(part)
    elapsed = time.monotonic() - t0
    r1 = stats_requests()
    total = sum(p["req"] for p in parts)
    bad = sum(p["bad"] for p in parts)
    n503 = sum(p["n503"] for p in parts)
    lats = sorted(x for p in parts for x in p["lat"])
    errs = [e for p in parts for e in p["errs"]]
    pct = lambda p: lats[min(len(lats) - 1, int(len(lats) * p))] if lats else 0.0
    print(f"conns={conns:<3} depth={depth:<4} "
          + (f"procs={procs} " if procs > 1 else "")
          + f"client_rps={total/elapsed:>9.0f} "
          f"server_rps={(r1-r0)/elapsed:>9.0f} 503={n503:<8} mismatch={bad:<7} "
          f"batch_p50={pct(.50)*1e3:>8.3f}ms batch_p99={pct(.99)*1e3:>9.3f}ms "
          f"per_req_p50={pct(.50)/depth*1e6:>7.1f}us"
          + (f" ERR={errs[:2]}" if errs else ""), flush=True)


if __name__ == "__main__":
    bench(int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]),
          int(sys.argv[4]) if len(sys.argv) > 4 else 1)
