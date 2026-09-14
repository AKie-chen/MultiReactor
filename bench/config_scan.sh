#!/usr/bin/env bash
# 重扫服务端 -i/-w 配置：换了 vCPU 数（或换了机器）后，用它在当前机器上重新找
# 最优 IO 线程数 / worker 数。4 vCPU 上的结论是 i=4 w=1，核数变了要重测。
#
# 每档跑两个指标：闭环 wrk（-c500）+ 流水线 16x256（2 进程客户端）。
# 脚本自己起停服务端，因此运行前必须没有别的实例占用端口。
#
# 用法: bash bench/config_scan.sh [port] [static_dir]
set -u
PORT=${1:-8080}
DIR=${2:-}
cd "$(dirname "$0")/.."
URL="http://127.0.0.1:${PORT}/user/123"
NCPU=$(nproc)
WT=$(( NCPU > 4 ? 8 : 4 ))   # 压测端线程数：核多时给足，避免客户端先成为瓶颈
# 流水线探针用 32x512 单进程：这是 8 核上接近峰值的形状，对服务端配置最敏感
# （16x256 太轻，多进程客户端反而更慢，会把配置差异淹没在客户端噪声里）

if curl -sf -o /dev/null "$URL" 2>/dev/null; then
  echo "错误：$URL 已有服务在跑，请先停掉（pkill -x multireactor）再运行本脚本" >&2
  exit 1
fi

start() {
  if [ -n "$DIR" ]; then
    ./build/multireactor --port "$PORT" "$@" -d "$DIR" >/tmp/cfg_scan_server.log 2>&1 &
  else
    ./build/multireactor --port "$PORT" "$@" >/tmp/cfg_scan_server.log 2>&1 &
  fi
  SRV=$!
  for _ in $(seq 1 50); do
    curl -sf -o /dev/null "$URL" 2>/dev/null && return 0
    sleep 0.1
  done
  echo "服务端未就绪，见 /tmp/cfg_scan_server.log" >&2
  return 1
}
stop() { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }

echo "vCPU=$NCPU  客户端: wrk -t$WT -c500 10s / 流水线 32x512 5s"
printf "%-12s %-12s %-12s\n" 配置 闭环_c500 流水线_32x512
for W in 1 2 3; do
  for I in 4 6 8; do
    [ "$I" -gt "$NCPU" ] && continue
    start -i "$I" -w "$W" || exit 1
    A=$(wrk -t$WT -c500 -d10s "$URL" | awk '/Requests\/sec/{print $2}')
    B=$(python3 bench/pipeline_bench.py 32 512 5 1 \
        | awk '{for(i=1;i<=NF;i++) if($i ~ /^client_rps=/){sub("client_rps=","",$i); print ($i==""?$(i+1):$i)}}')
    stop
    printf "%-12s %-12s %-12s\n" "i=$I w=$W" "${A:-ERR}" "${B:-ERR}"
    sleep 0.5
  done
done
echo "（结果只在本机内可比；绝对值随 vCPU 数与客户端强度变化）"
