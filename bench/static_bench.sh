#!/usr/bin/env bash
# 静态文件吞吐测试：小文件（≤64KB）走 LRU 内容缓存，大文件走 sendfile 零拷贝。
#
# 前置：服务端以静态目录启动，例如
#   ./build/multireactor --port 8080 -d /tmp/bench_static &
# 测试文件由本脚本生成（随机内容，落在 /tmp，不入仓库）。
#
# 用法: bash bench/static_bench.sh [port] [dir]
set -u
PORT=${1:-8080}
DIR=${2:-/tmp/bench_static}
BASE="http://127.0.0.1:${PORT}"

mkdir -p "$DIR"
for s in 1 4 16 64 128 300; do
  [ -f "$DIR/f${s}k.bin" ] || head -c $((s * 1024)) /dev/urandom > "$DIR/f${s}k.bin"
done
[ -f "$DIR/f1m.bin" ] || head -c 1048576 /dev/urandom > "$DIR/f1m.bin"
[ -f "$DIR/f4m.bin" ] || head -c 4194304 /dev/urandom > "$DIR/f4m.bin"

code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/f1k.bin")
if [ "$code" != "200" ]; then
  echo "错误：$BASE/f1k.bin 返回 $code —— 服务端是否以 -d $DIR 启动？" >&2
  exit 1
fi

sweep() { # $1=标题 $2..=文件
  echo "=== $1 ==="
  printf "%-8s %-11s %-10s %-9s %-9s\n" 文件 吞吐req/s 带宽 P50 P99
  shift
  for f in "$@"; do
    O=$(wrk -t4 -c100 -d10s --latency "$BASE/$f.bin")
    printf "%-8s %-11s %-10s %-9s %-9s\n" "$f" \
      "$(echo "$O" | grep 'Requests/sec:' | awk '{print $2}')" \
      "$(echo "$O" | grep 'Transfer/sec:' | awk '{print $2}')" \
      "$(echo "$O" | grep '^ *50%' | awk '{print $2}')" \
      "$(echo "$O" | grep '^ *99%' | awk '{print $2}')"
  done
}

sweep "小文件（≤64KB，LRU 缓存路径，-c100）" f1k f4k f16k f64k
sweep "大文件（>64KB，sendfile 零拷贝路径，-c100）" f128k f300k f1m f4m

echo "=== 304 协商缓存（If-Modified-Since，无 body，-c100）==="
LM=$(curl -s -I "$BASE/f1k.bin" | grep -i last-modified | sed 's/^[Ll]ast-[Mm]odified: //' | tr -d '\r')
if [ -z "$LM" ]; then
  echo "（未取到 Last-Modified，跳过）"
else
  for f in f1k f64k; do
    O=$(wrk -t4 -c100 -d10s --latency -H "If-Modified-Since: $LM" "$BASE/$f.bin")
    printf "%-8s %-11s %-10s P50=%s\n" "$f  304" \
      "$(echo "$O" | grep 'Requests/sec:' | awk '{print $2}')" \
      "$(echo "$O" | grep 'Transfer/sec:' | awk '{print $2}')" \
      "$(echo "$O" | grep '^ *50%' | awk '{print $2}')"
  done
fi
