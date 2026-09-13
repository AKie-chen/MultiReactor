#!/usr/bin/env python3
"""逐字节拆包测试：把 K 个流水线请求的 payload 在**每个字节位置**切成两段发送，
检查服务端是否始终返回全部 200（任何非 200 或提前关连接都视为缺陷）。

覆盖 HttpContext 状态机的跨 TCP 拆包路径——单测里的拆包用例是有限的，
这里穷举一个流水线批次的每个切分点，配合 0ms/1ms 两种间隔（后者强制两个 TCP
段，前者往往仍被合并，两者结合可覆盖"内核合并"与"真拆包"两种情形）。

用法: split_fuzz.py <K> <间隔ms>
示例: python3 bench/split_fuzz.py 8 1
"""
import socket, time, sys

HOST, PORT = "127.0.0.1", 8080


def read_responses(sock, n, budget=1.0):
    """解析最多 n 个响应，返回 (状态码列表, 是否被提前关闭)。"""
    buf = b""
    codes = []
    sock.settimeout(budget)
    try:
        while len(codes) < n:
            while b"\r\n\r\n" not in buf:
                c = sock.recv(4096)
                if not c:
                    return codes, True          # 服务端关闭
                buf += c
            he = buf.find(b"\r\n\r\n") + 4
            cl = 0
            for line in buf[:he].split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    cl = int(line.split(b":")[1])
            while len(buf) < he + cl:
                c = sock.recv(4096)
                if not c:
                    return codes, True
                buf += c
            codes.append(buf[9:12])
            buf = buf[he + cl:]
    except socket.timeout:
        return codes, False
    return codes, False


def main(k, sleep_ms):
    req = b"GET /user/1234 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
    payload = req * k
    bad = 0
    for p in range(1, len(payload)):
        s = socket.create_connection((HOST, PORT))
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.sendall(payload[:p])
        if sleep_ms:
            time.sleep(sleep_ms / 1000.0)
        s.sendall(payload[p:])
        codes, closed = read_responses(s, k)
        s.close()
        if len(codes) != k or any(c != b"200" for c in codes):
            bad += 1
            if bad <= 5:
                print(f"  [缺陷] split@{p}: 收到 {len(codes)}/{k} 响应, "
                      f"状态码={[c.decode() for c in codes]}, 提前关闭={closed}")
    print(f"K={k} 间隔={sleep_ms}ms: 共 {len(payload)-1} 个切分点, 异常 {bad} 个")


if __name__ == "__main__":
    main(int(sys.argv[1]), float(sys.argv[2]))
