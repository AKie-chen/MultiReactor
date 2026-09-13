#!/usr/bin/env python3
"""流水线基准的"诊断版"：负载与 pipeline_bench.py 相同，但用于定位偶发缺陷。

与基准版的差别（见 bench/README.md 第 7 节）：
  - 任何一批校验失败 → 原始字节 dump 到 /tmp/bench/anomaly_conn<N>.bin
  - 统计**全部**状态码直方图（不只 200/503），非 200 也能看到是什么
  - 打印服务端 /stats 的 requests 与 err_5xx 增量，与服务端计数对账

静默 5xx 排查技巧：wrk/wrk2 的输出不含状态码统计，不主动测增量会完全看不到
503/505/501；本脚本每次运行都会打印 err_5xx 增量。

用法: pipeline_diag.py <conns> <depth> <duration_s>
"""
import socket, sys, time, json, threading, urllib.request, collections

HOST, PORT = "127.0.0.1", 8080
ID_BASE = 1000
DUMP_DIR = "/tmp/bench"
DUMPED = threading.Event()


def stats():
    with urllib.request.urlopen(f"http://{HOST}:{PORT}/stats", timeout=5) as r:
        return json.load(r)


def parse_all(buf, hist):
    """解析缓冲区中所有完整响应，累计状态码直方图；返回剩余（不完整）字节。"""
    while b"\r\n\r\n" in buf:
        head_end = buf.find(b"\r\n\r\n") + 4
        clen = 0
        for line in bytes(buf[:head_end]).split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                clen = int(line.split(b":")[1])
        if len(buf) < head_end + clen:
            break
        hist[bytes(buf[9:12])] += 1
        del buf[:head_end + clen]
    return buf


def probe_stride():
    s = socket.create_connection((HOST, PORT))
    s.sendall(f"GET /user/{ID_BASE} HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n".encode())
    b = b""
    while b"\r\n\r\n" not in b:
        b += s.recv(4096)
    he = b.find(b"\r\n\r\n") + 4
    cl = int([l for l in b[:he].split(b"\r\n")
              if l.lower().startswith(b"content-length")][0].split(b":")[1])
    s.close()
    return he + cl, he


def run_conn(depth, deadline, stride, body_off, sid, out):
    ids = [ID_BASE + i for i in range(depth)]
    payload = b"".join(f"GET /user/{i} HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n".encode() for i in ids)
    exp = [f"user id: {i}".encode() for i in ids]
    need = depth * stride
    hist = collections.Counter()
    n_bad = n_ok = 0
    slow = False
    s = socket.create_connection((HOST, PORT))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(5)
    try:
        while time.monotonic() < deadline:
            s.sendall(payload)
            buf = bytearray()
            while len(buf) < need:
                c = s.recv(262144)
                if not c:
                    raise ConnectionError("server closed")
                buf += c
            if not slow:
                bad_here = 0
                for j in range(depth):
                    off = j * stride
                    if buf[off:off + 15] != b"HTTP/1.1 200 OK" or \
                       buf[off + body_off:off + stride] != exp[j]:
                        bad_here += 1
                if bad_here:
                    n_bad += bad_here
                    if not DUMPED.is_set():
                        DUMPED.set()
                        with open(f"{DUMP_DIR}/anomaly_conn{sid}.bin", "wb") as f:
                            f.write(bytes(buf))
                    parse_all(bytearray(buf), hist)  # 本批全状态码
                    slow = True                      # 后续批次持续走慢路径统计
                    continue
                n_ok += depth
            else:
                before = hist.get(b"200", 0)
                parse_all(buf, hist)
                n_ok += hist.get(b"200", 0) - before
    except Exception as e:
        out[sid] = {"err": type(e).__name__, "bad": n_bad, "ok": n_ok, "hist": dict(hist)}
        s.close(); return
    s.close()
    out[sid] = {"bad": n_bad, "ok": n_ok, "hist": dict(hist)}


def main(conns, depth, dur):
    b0 = stats()
    stride, body_off = probe_stride()
    out = [None] * conns
    t0 = time.monotonic(); dl = t0 + dur
    ths = [threading.Thread(target=run_conn, args=(depth, dl, stride, body_off, i, out))
           for i in range(conns)]
    for t in ths: t.start()
    for t in ths: t.join()
    el = time.monotonic() - t0
    b1 = stats()
    ok = sum(r["ok"] for r in out)
    bad = sum(r["bad"] for r in out)
    hist = collections.Counter()
    for r in out:
        hist.update(r["hist"])
    errs = collections.Counter(r["err"] for r in out if "err" in r)
    print(f"conns={conns:<4} depth={depth:<4} ok_rps={ok/el:>9.0f} "
          f"服务端requests增量={b1['requests']-b0['requests']:<9} "
          f"server_5xx增量={b1['err_5xx']-b0['err_5xx']:<6} "
          f"bad={bad:<7} 状态码直方图={dict(hist)} 错误={dict(errs)}", flush=True)


if __name__ == "__main__":
    main(int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]))
