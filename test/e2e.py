#!/usr/bin/env python3
"""
MultiReactor 端到端回归测试 —— 真实进程 + 真实 socket + 逐字节校验。

覆盖什么：
    单元测试（test/*.cpp）只覆盖不依赖事件循环与网络的纯逻辑模块
    （HttpContext 解析、Router、StaticFileHandler、ThreadPool）。
    本脚本启动真实服务端进程，用裸 socket 发原始报文，补齐中间那层：

      1. 动态路由      —— 三条路由响应体逐字节、/stats 计数对账、404/405、HEAD
      2. 协议行为      —— 400/413/417/501/505 的确切触发条件
      3. 静态文件      —— LRU 缓存路径与 sendfile 路径逐字节相等、304、穿越防护
      4. 连接生命周期  —— keep-alive 复用、响应后及时关闭（计时）、Connection: close
      5. 流水线保序    —— 同连接连发多请求，响应顺序与请求一一对应
      6. 并发与优雅关闭 —— 50×20 并发、SIGTERM 排空期在途响应完整送达

    第 4、5、6 组对应 README「踩坑记录」里的真实回归项：短连接响应后不关闭、
    流水线响应错配、优雅关闭排空期丢弃请求。

为什么用裸 socket 而不是 curl：
    逐字节校验要求拿到未经规范化的原始报文；连接关闭时刻（EOF）与流水线保序
    根本无法用 curl 表达。仅依赖 Python 标准库，与项目「C++ 零第三方依赖」一致。

用法:
    python3 test/e2e.py                    # 默认用 build/multireactor
    python3 test/e2e.py path/to/multireactor
    ctest --test-dir build                 # 与单元测试一起跑

退出码:
    0 = 全部通过，1 = 有失败项。
    任何情况下都会 SIGTERM 服务端并删除临时目录（try/finally 保证），
    不留残留进程与文件。
"""

import json, os, shutil, signal, socket, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = "127.0.0.1"
KIB = 1024

# 静态夹具尺寸：刻意跨过 StaticFileHandler 的 64KB 分界线，
# 让小文件走内存 LRU 缓存路径、大文件走 sendfile 零拷贝路径
SMALL_SIZE = 30 * KIB
BIG_SIZE = 4 * 1024 * KIB
SMALL_BYTES = bytes((i * 7 + 11) % 256 for i in range(SMALL_SIZE))
BIG_BYTES = bytes((i * 13 + 29) % 256 for i in range(BIG_SIZE))

FIXTURES = {
    "hello.txt": b"hello from fixture\n",
    "small.bin": SMALL_BYTES,
    "big.bin": BIG_BYTES,
    "sub/index.html": b"<html><body>index fixture</body></html>",
}


# ---------------------------------------------------------------- HTTP 解析

class Response:
    __slots__ = ("code", "reason", "headers", "header_order", "body")

    def __init__(self, code, reason, headers, header_order, body):
        self.code = code
        self.reason = reason
        self.headers = headers          # 小写键 -> bytes 值
        self.header_order = header_order  # 原始顺序的键名 list（小写）
        self.body = body

    def __repr__(self):
        return "Response(%d %s, %d bytes body)" % (self.code, self.reason, len(self.body))


def read_response(sock, buf, expect_body=True):
    """从 buf 中解析一个完整响应并就地消费，返回 (Response, 剩余 buf)。

    buf 是调用方持有的 bytearray —— 就地消费是流水线解析的前提。
    按 Content-Length 定界（服务端不发 chunked，TE 请求直接 501）。
    """
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            raise AssertionError("连接在响应头完整到达前被关闭（已收 %d 字节）" % len(buf))
        buf += chunk

    head_end = buf.find(b"\r\n\r\n")
    lines = bytes(buf[:head_end]).split(b"\r\n")

    parts = lines[0].split(b" ", 2)
    if len(parts) < 2:
        raise AssertionError("畸形状态行: %r" % lines[0])
    code = int(parts[1])
    reason = parts[2].decode("latin-1") if len(parts) > 2 else ""

    headers, order = {}, []
    for line in lines[1:]:
        key, sep, val = line.partition(b":")
        if not sep:
            raise AssertionError("畸形响应头（无冒号）: %r" % line)
        k = key.strip().lower()
        headers[k] = val.strip()
        order.append(k)

    body_start = head_end + 4
    # 1xx/204/304 无 body；HEAD 由调用方用 expect_body=False 跳过
    if not expect_body or code < 200 or code in (204, 304):
        body = b""
    else:
        clen = int(headers.get(b"content-length", b"0"))
        while len(buf) < body_start + clen:
            chunk = sock.recv(65536)
            if not chunk:
                raise AssertionError(
                    "响应体不完整：期望 %d 字节，实收 %d 字节" % (clen, len(buf) - body_start))
            buf += chunk
        body = bytes(buf[body_start:body_start + clen])

    consumed = body_start + len(body)
    del buf[:consumed]
    return Response(code, reason, headers, order, body), buf


# ---------------------------------------------------------------- 服务端进程

class Server:
    def __init__(self, binary):
        self.binary = binary
        self.port = self._free_port()
        self.static_dir = tempfile.mkdtemp(prefix="mr_e2e_")
        # 日志放在临时目录之外（清理时不会被 rmtree 带走），并以随机目录名区分，
        # 避免并发运行同一脚本时互相覆盖
        self.log_path = os.path.join(
            tempfile.gettempdir(), os.path.basename(self.static_dir) + ".log")
        self.proc = None
        self.log_file = None

    @staticmethod
    def _free_port():
        """探测一个空闲端口后释放再用。避免 bench 脚本固定 8080 的冲突问题。"""
        s = socket.socket()
        s.bind((HOST, 0))
        port = s.getsockname()[1]
        s.close()
        return port

    def _write_fixtures(self):
        for rel, content in FIXTURES.items():
            path = os.path.join(self.static_dir, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(content)

    def start(self):
        if not os.path.isfile(self.binary):
            raise SystemExit(
                "错误：找不到服务端可执行文件 %s\n"
                "请先构建：cmake --build build -j$(nproc)" % self.binary)

        self._write_fixtures()
        self.log_file = open(self.log_path, "wb")
        self.proc = subprocess.Popen(
            [self.binary, "-p", str(self.port), "-d", self.static_dir,
             "-i", "2", "-w", "2", "--log-level", "ERROR"],
            stdout=self.log_file, stderr=subprocess.STDOUT,
            cwd=ROOT)

        # 轮询 connect 判 ready，不用 sleep 定值（比被删 CI 里的 sleep 2 可靠）
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise SystemExit(
                    "错误：服务端启动即退出（退出码 %s），日志见 %s"
                    % (self.proc.returncode, self.log_path))
            try:
                socket.create_connection((HOST, self.port), timeout=0.5).close()
                return
            except OSError:
                time.sleep(0.05)
        raise SystemExit("错误：服务端 10 秒内未就绪，日志见 %s" % self.log_path)

    def connect(self, timeout=5, rcvbuf=None):
        s = socket.create_connection((HOST, self.port), timeout=timeout)
        if rcvbuf is not None:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        return s

    def stop(self):
        """SIGTERM 触发优雅关闭；已在排空/已退出时忽略。"""
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)

    def cleanup(self):
        self.stop()
        if self.log_file is not None:
            self.log_file.close()
        if os.path.isdir(self.static_dir):
            shutil.rmtree(self.static_dir, ignore_errors=True)


SERVER = None


def request(method=b"GET", path=b"/", headers=()):
    """用一次性短连接发一个请求并读回响应。"""
    sock = SERVER.connect()
    payload = method + b" " + path + b" HTTP/1.1\r\nHost: e2e\r\n"
    for k, v in headers:
        payload += k + b": " + v + b"\r\n"
    payload += b"\r\n"
    sock.sendall(payload)
    resp, _ = read_response(sock, bytearray(), expect_body=(method != b"HEAD"))
    sock.close()
    return resp


def read_until_eof(sock):
    """读到 EOF，返回 (全部字节, 耗时秒)。

    只可用于服务端会主动关闭的连接（请求带 Connection: close，或连接被 reset）；
    对 keep-alive 连接调用会一直阻塞到 socket 超时。
    """
    t0 = time.time()
    data = b""
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        data += chunk
    return data, time.time() - t0


# ---------------------------------------------------------------- 检查注册

CHECKS = []


def check(name):
    def deco(fn):
        CHECKS.append((name, fn))
        return fn
    return deco


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def stats():
    return request(path=b"/stats").body


def stats_field(name):
    return json.loads(stats())[name]


# ================================================== 1. 动态路由

@check("RootRoute")
def _():
    r = request(path=b"/")
    expect(r.code == 200, "状态码 %d != 200" % r.code)
    expect(r.body == b"Hello, World!", "body %r != b'Hello, World!'" % r.body)
    expect(r.headers.get(b"content-length") == b"13",
           "Content-Length %r != b'13'" % r.headers.get(b"content-length"))
    expect(r.headers.get(b"content-type") == b"text/plain",
           "Content-Type %r != b'text/plain'" % r.headers.get(b"content-type"))
    # 结构不变量：Date 由序列化层单独置于最前，业务头在其后，Connection 永远最后
    expect(r.header_order[0] == b"date", "首个响应头应为 Date，实际 %r" % r.header_order[0])


@check("ParamRoute")
def _():
    r = request(path=b"/user/42")
    expect(r.code == 200, "状态码 %d != 200" % r.code)
    expect(r.body == b"user id: 42", "body %r != b'user id: 42'" % r.body)


@check("StatsJsonShape")
def _():
    r = request(path=b"/stats")
    expect(r.code == 200, "状态码 %d != 200" % r.code)
    expect(r.headers.get(b"content-type") == b"application/json",
           "Content-Type %r" % r.headers.get(b"content-type"))
    obj = json.loads(r.body)
    want = {"requests", "active", "err_4xx", "err_5xx", "bytes_recv", "bytes_sent"}
    expect(set(obj) == want, "字段集合 %r != %r" % (sorted(obj), sorted(want)))
    expect(all(isinstance(v, int) for v in obj.values()), "存在非整数字段: %r" % obj)


@check("StatsAccounting")
def _():
    # 解析失败的请求不计入 requests（main.cpp 在解析成功后自增），
    # 所以发 N 个成功请求后增量应精确等于 N + 读取 /stats 自身那一次
    n = 50
    before = stats_field("requests")
    for _ in range(n):
        request(path=b"/user/7").body
    after = stats_field("requests")
    expect(after - before == n + 1,
           "requests 增量 %d != %d（含 /stats 自身一次）" % (after - before, n + 1))


@check("NotFound")
def _():
    r = request(path=b"/no/such/path")
    expect(r.code == 404, "状态码 %d != 404" % r.code)
    expect(b"<h1>404 Not Found</h1>" in r.body, "错误响应体不含 404 标题: %r" % r.body)
    expect(r.headers.get(b"connection") == b"close",
           "错误响应应带 Connection: close")


@check("MethodNotAllowedRoute")
def _():
    # 路由层 405：path 已注册但方法不匹配，reason 是 "Method Not Allowed"
    r = request(method=b"POST", path=b"/", headers=[(b"Content-Length", b"0")])
    expect(r.code == 405, "状态码 %d != 405" % r.code)
    expect(r.reason == "Method Not Allowed", "reason %r != 'Method Not Allowed'" % r.reason)


@check("UnknownMethod")
def _():
    # 解析层 405：方法本身不认识，reason 是 "Method Not Supported"（与上面不同！）
    r = request(method=b"DELETE", path=b"/")
    expect(r.code == 405, "状态码 %d != 405" % r.code)
    expect(r.reason == "Method Not Supported", "reason %r != 'Method Not Supported'" % r.reason)


@check("HeadEqualsGet")
def _():
    # HEAD 按 GET 查表，只发头，但保留真实 Content-Length
    r = request(method=b"HEAD", path=b"/")
    expect(r.code == 200, "状态码 %d != 200" % r.code)
    expect(r.body == b"", "HEAD 不应有响应体，实收 %d 字节" % len(r.body))
    expect(r.headers.get(b"content-length") == b"13",
           "HEAD 应保留真实 Content-Length: 13，实际 %r" % r.headers.get(b"content-length"))


# ================================================== 2. 协议行为

def _expect_error(raw_request, code, keyword, label):
    """发一段原始报文，断言状态码与错误页关键字。"""
    s = SERVER.connect()
    s.sendall(raw_request)
    resp, _ = read_response(s, bytearray())
    s.close()
    expect(resp.code == code, "%s：状态码 %d != %d" % (label, resp.code, code))
    expect(keyword.encode() in resp.body,
           "%s：响应体不含 %r（实为 %r）" % (label, keyword, resp.body))
    expect(resp.headers.get(b"connection") == b"close",
           "%s：错误响应应带 Connection: close" % label)


@check("MissingHost")
def _():
    _expect_error(b"GET / HTTP/1.1\r\n\r\n", 400, "400 Bad Request", "缺 Host")


@check("EmptyHost")
def _():
    _expect_error(b"GET / HTTP/1.1\r\nHost:\r\n\r\n", 400, "400 Bad Request", "空 Host")


@check("DuplicateHost")
def _():
    _expect_error(b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
                  400, "400 Bad Request", "重复 Host")


@check("ConflictingContentLength")
def _():
    _expect_error(
        b"GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
        400, "400 Bad Request", "CL+CL 冲突")


@check("NonNumericContentLength")
def _():
    _expect_error(b"GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5abc\r\n\r\n",
                  400, "400 Bad Request", "CL 非数字")


@check("HeaderWithoutColon")
def _():
    _expect_error(b"GET / HTTP/1.1\r\nHost: a\r\nBadHeaderNoColon\r\n\r\n",
                  400, "400 Bad Request", "头行无冒号")


@check("TransferEncodingRejected")
def _():
    # 踩坑记录：TE 被静默忽略时，chunked 帧字节会被当成新请求解析（请求走私向量）
    _expect_error(b"POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n",
                  501, "501 Not Implemented", "Transfer-Encoding")


@check("UnsupportedExpectation")
def _():
    _expect_error(b"GET / HTTP/1.1\r\nHost: a\r\nExpect: nonsense\r\n\r\n",
                  417, "417 Expectation Failed", "Expect 无法满足")


@check("OversizeHeaderLine")
def _():
    _expect_error(b"GET / HTTP/1.1\r\nHost: a\r\nX-Pad: " + b"A" * 9000 + b"\r\n\r\n",
                  413, "413 Request Header Too Large", "超长头行（>8192）")


@check("OversizeContentLength")
def _():
    _expect_error(b"GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 100000000\r\n\r\n",
                  413, "413 Request Header Too Large", "CL 超 16MB 上限")


@check("HttpVersionNotSupported")
def _():
    _expect_error(b"GET / HTTP/2.0\r\nHost: a\r\n\r\n",
                  505, "505 Http Version Not Supported", "HTTP/2.0")


@check("Expect100Continue")
def _():
    # Content-Length > 0 时先回一个裸 100 Continue，收完 body 再回真实响应
    s = SERVER.connect()
    s.sendall(b"POST /user/9 HTTP/1.1\r\nHost: a\r\n"
              b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n")
    buf = bytearray()
    interim, buf = read_response(s, buf)
    expect(interim.code == 100, "首个响应 %d != 100" % interim.code)
    expect(interim.body == b"", "100 Continue 不应有 body")
    s.sendall(b"12345")
    final, _ = read_response(s, buf)
    s.close()
    expect(final.code == 405, "最终状态码 %d != 405（POST 未注册）" % final.code)


@check("ExpectContinueOrderedBefore413")
def _():
    # 限长检查先于状态转移，所以超大 CL 直接 413，绝不能先发 100 Continue——
    # 否则客户端会白白上传 100MB 才被告知拒绝
    s = SERVER.connect()
    s.sendall(b"POST / HTTP/1.1\r\nHost: a\r\n"
              b"Content-Length: 100000000\r\nExpect: 100-continue\r\n\r\n")
    resp, _ = read_response(s, bytearray())
    s.close()
    expect(resp.code == 413, "状态码 %d != 413（收到 100 说明限长检查被绕过）" % resp.code)


# ================================================== 3. 静态文件

@check("StaticSmallFileBytes")
def _():
    # ≤64KB 走内存 LRU 缓存路径
    r = request(path=b"/small.bin")
    expect(r.code == 200, "状态码 %d != 200" % r.code)
    expect(r.body == SMALL_BYTES,
           "小文件字节不一致：%d 字节 vs 期望 %d" % (len(r.body), len(SMALL_BYTES)))


@check("StaticLargeFileBytes")
def _():
    # >64KB 走 sendfile 零拷贝路径
    r = request(path=b"/big.bin")
    expect(r.code == 200, "状态码 %d != 200" % r.code)
    expect(len(r.body) == BIG_SIZE,
           "大文件长度 %d != %d" % (len(r.body), BIG_SIZE))
    expect(r.body == BIG_BYTES, "大文件字节不一致（sendfile 路径）")


@check("StaticIndexHtml")
def _():
    r = request(path=b"/sub/")
    expect(r.code == 200, "状态码 %d != 200" % r.code)
    expect(r.body == FIXTURES["sub/index.html"], "index.html 内容不符: %r" % r.body)


@check("StaticNotModified")
def _():
    first = request(path=b"/hello.txt")
    lm = first.headers.get(b"last-modified")
    expect(lm is not None, "首次响应应含 Last-Modified")
    r = request(path=b"/hello.txt", headers=[(b"If-Modified-Since", lm)])
    expect(r.code == 304, "状态码 %d != 304" % r.code)
    # 304 不得带 body 与 Content-Length
    expect(r.body == b"", "304 不应有 body，实收 %d 字节" % len(r.body))
    expect(b"content-length" not in r.headers,
           "304 不应带 Content-Length，实际 %r" % r.headers.get(b"content-length"))


@check("StaticTraversalBlocked")
def _():
    for path in (b"/../etc/passwd", b"/%2e%2e/etc/passwd", b"/sub/../../etc/passwd"):
        r = request(path=path)
        expect(r.code == 403, "路径 %r 状态码 %d != 403（穿越未被拦截）" % (path, r.code))


@check("StaticMissing")
def _():
    r = request(path=b"/does-not-exist.txt")
    expect(r.code == 404, "状态码 %d != 404" % r.code)


# ================================================== 4. 连接生命周期

@check("KeepAliveReuse")
def _():
    s = SERVER.connect()
    buf = bytearray()
    for i in range(5):
        s.sendall(b"GET /user/%d HTTP/1.1\r\nHost: a\r\n\r\n" % i)
        r, buf = read_response(s, buf)
        expect(r.code == 200, "第 %d 个请求状态码 %d" % (i, r.code))
        expect(r.body == b"user id: %d" % i, "第 %d 个响应体 %r" % (i, r.body))
    s.close()


@check("ConnectionCloseHonored")
def _():
    s = SERVER.connect()
    s.sendall(b"GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n")
    data, _ = read_until_eof(s)
    s.close()
    expect(b"Connection: close" in data, "响应应含 Connection: close")
    expect(data.endswith(b"Hello, World!"), "响应体不完整: %r" % data[-40:])


@check("ShortConnectionClosesPromptly")
def _():
    # 踩坑记录：直接写成功路径没有 EPOLLOUT 事件，maybeCloseAfterSend 被跳过，
    # 连接一直挂到心跳超时才关。修复后应立即 EOF——这里计时守住该回归。
    s = SERVER.connect()
    s.sendall(b"GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n")
    data, elapsed = read_until_eof(s)
    s.close()
    expect(data.endswith(b"Hello, World!"), "响应不完整")
    expect(elapsed < 1.0,
           "响应后 %.3f 秒才关闭连接（应即时关闭，挂到心跳超时说明回归）" % elapsed)


@check("KeepAliveStaysOpen")
def _():
    s = SERVER.connect()
    s.sendall(b"GET / HTTP/1.1\r\nHost: a\r\n\r\n")
    read_response(s, bytearray())
    s.settimeout(1.0)
    try:
        extra = s.recv(100)
    except socket.timeout:
        extra = None  # 超时 = 仍保持打开，符合预期
    s.close()
    expect(extra is None or extra == b"",
           "keep-alive 连接在 1 秒内被意外关闭: %r" % extra)


# ================================================== 5. 流水线保序

@check("PipelineOrderUniform")
def _():
    # 踩坑记录：多 worker 乱序完成导致响应错配（[200,400] → [400,400]）。
    # 定长 id 使所有响应等长，便于逐字节核对顺序。
    n = 32
    ids = [1000 + i for i in range(n)]
    s = SERVER.connect()
    s.sendall(b"".join(b"GET /user/%d HTTP/1.1\r\nHost: a\r\n\r\n" % i for i in ids))
    buf = bytearray()
    for idx, i in enumerate(ids):
        r, buf = read_response(s, buf)
        expect(r.code == 200, "第 %d 个响应状态码 %d" % (idx, r.code))
        expect(r.body == b"user id: %d" % i,
               "第 %d 个响应错配：期望 'user id: %d'，实际 %r" % (idx, i, r.body))
    s.close()


@check("PipelineMixedStatusOrder")
def _():
    # 混合命中与未命中，验证状态码序列与请求序列严格一一对应
    paths = [b"/user/1", b"/nope", b"/user/2", b"/nope", b"/user/3"]
    wants = [200, 404, 200, 404, 200]
    s = SERVER.connect()
    buf = bytearray()
    for p in paths:
        s.sendall(b"GET " + p + b" HTTP/1.1\r\nHost: a\r\n\r\n")
    got = []
    for _ in paths:
        r, buf = read_response(s, buf)
        got.append(r.code)
    s.close()
    expect(got == wants, "状态码序列 %r != %r（流水线响应错配）" % (got, wants))


# ================================================== 6. 并发与优雅关闭

@check("Concurrent50x20")
def _():
    # README 声称的 50 连接 × 20 请求
    n_conn, n_req = 50, 20
    errors = []
    lock = threading.Lock()

    def worker(cid):
        try:
            s = SERVER.connect(timeout=10)
            buf = bytearray()
            for i in range(n_req):
                uid = cid * 1000 + i
                s.sendall(b"GET /user/%d HTTP/1.1\r\nHost: a\r\n\r\n" % uid)
                r, buf = read_response(s, buf)
                if r.code != 200 or r.body != b"user id: %d" % uid:
                    with lock:
                        errors.append("连接 %d 第 %d 个: %d %r" % (cid, i, r.code, r.body))
                    return
            s.close()
        except Exception as e:
            with lock:
                errors.append("连接 %d: %s" % (cid, e))

    threads = [threading.Thread(target=worker, args=(c,)) for c in range(n_conn)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=30)
    expect(not errors, "%d/%d 条连接出错，前 3 个: %r"
           % (len(errors), n_conn, errors[:3]))


# SIGTERM 后 listen fd 即关闭，新连接会被拒——所以排空期的检查必须复用
# 在 SIGTERM 之前就已建立的连接。此变量由 ShutdownDrainsInflight 填充。
_IDLE_CONN = None


@check("ShutdownDrainsInflight")
def _():
    # 排空期在途响应必须完整送达（踩坑记录：排空期请求被直接丢弃，客户端挂起）。
    # 用 4MB 文件 + 收窄 SO_RCVBUF，确保 SIGTERM 到达时该响应确实还在传输中。
    #
    # 注意：必须按 Content-Length 定界读，不能用「读到 EOF」——该请求在 SIGTERM
    # 之前就已由 worker 处理，响应不带 Connection: close，服务端发完不会关闭连接；
    # 等 EOF 会一路阻塞到 10 秒排空兜底强关，把在途响应测试变成兜底路径测试。
    global _IDLE_CONN
    _IDLE_CONN = SERVER.connect(timeout=15)   # 供下一项检查复用，必须此刻建立

    s = SERVER.connect(timeout=15, rcvbuf=64 * KIB)
    s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: a\r\n\r\n")
    time.sleep(0.2)                       # 让服务端开始推送、并阻塞在 EAGAIN 上

    SERVER.proc.send_signal(signal.SIGTERM)
    time.sleep(0.2)                       # 确认排空已开始而非已结束

    r, _ = read_response(s, bytearray())
    s.close()
    expect(r.code == 200, "排空期在途响应状态码 %d != 200" % r.code)
    expect(len(r.body) == BIG_SIZE,
           "排空期在途响应不完整：%d 字节，期望 %d" % (len(r.body), BIG_SIZE))
    expect(r.body == BIG_BYTES, "排空期在途响应字节不一致")


# 最后一条连接关闭的时刻，用于测量「连接归零 → 进程退出」的排空收敛延迟
_DRAIN_T0 = None


@check("ShutdownServesExistingConn")
def _():
    # 排空期只关闭 listen fd，已建立连接照常服务；但响应强制 Connection: close
    global _DRAIN_T0
    expect(_IDLE_CONN is not None, "前置检查未建立连接")
    _IDLE_CONN.sendall(b"GET /user/555 HTTP/1.1\r\nHost: a\r\n\r\n")
    r, _ = read_response(_IDLE_CONN, bytearray())
    _IDLE_CONN.close()
    _DRAIN_T0 = time.time()               # 此刻活跃连接归零，排空应当收敛
    expect(r.code == 200, "排空期请求状态码 %d != 200（请求被丢弃）" % r.code)
    expect(r.body == b"user id: 555", "排空期响应体 %r" % r.body)
    expect(r.headers.get(b"connection") == b"close",
           "排空期响应应强制 Connection: close，实际 %r"
           % r.headers.get(b"connection"))


@check("ShutdownExitsPromptly")
def _():
    # 连接归零后应迅速退出，远早于 10 秒兜底 deadline（README 声称 0.1s 内）。
    # 从「最后一条连接关闭」起算而非从本项检查开始起算——排空收敛很快，
    # 进程很可能在上一个检查与这一个之间就已经退出了，用 poll() 判存活会偶发失败。
    expect(_DRAIN_T0 is not None, "前置检查未记录归零时刻")
    try:
        code = SERVER.proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        raise AssertionError("连接归零后 10 秒仍未退出（排空未收敛，走了兜底路径）")
    elapsed = time.time() - _DRAIN_T0
    expect(elapsed < 3.0,
           "连接归零后 %.2f 秒才退出（应远早于 10s 兜底 deadline）" % elapsed)
    expect(code == 0, "SIGTERM 优雅关闭应返回 0，实际 %s" % code)


# ---------------------------------------------------------------- 主流程

def main():
    global SERVER
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "multireactor")

    SERVER = Server(binary)
    log_path = SERVER.log_path
    failed = 0
    try:
        SERVER.start()
        for name, fn in CHECKS:
            try:
                fn()
            except AssertionError as e:
                print("[FAIL] %s — %s" % (name, e))
                failed += 1
            except Exception as e:
                print("[FAIL] %s — 异常 %s: %s" % (name, type(e).__name__, e))
                failed += 1
            else:
                print("[PASS] %s" % name)
    finally:
        SERVER.cleanup()

    total = len(CHECKS)
    print()
    print("%d/%d end-to-end checks passed" % (total - failed, total))
    if failed:
        print("服务端日志: %s" % log_path)   # 失败时保留日志供排查
    elif os.path.isfile(log_path):
        os.unlink(log_path)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
