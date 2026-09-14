#!/usr/bin/env bash
# 一键复跑全部性能测试（数据与解读见 bench/README.md）。
#
# 前置：服务端已启动。8 vCPU 上推荐的配置由 bench/config_scan.sh 扫出来：
#   ./build/multireactor --port 8080 -i 8 -w 1 -d /tmp/bench_static &
#
# 用法: bash bench/run_bench.sh [port] [outdir]
set -u
PORT=${1:-8080}
OUT=${2:-/tmp/bench_out}
URL="http://127.0.0.1:${PORT}/user/123"
T=$(( $(nproc) > 4 ? 8 : 4 ))   # wrk 线程数随核数走，否则客户端先饱和
mkdir -p "$OUT"
cd "$(dirname "$0")/.."

echo "=== 1/5 闭环基线 (wrk -t$T, 10s x 2 轮) ==="
for C in 100 500 1000 2000 5000; do
  for r in 1 2; do
    echo -n "c=$C run$r: "
    wrk -t$T -c$C -d10s --latency "$URL" \
      | grep -E "Requests/sec|Non-2xx|^ *50%|^ *99%" | tr '\n' ' '
    echo
  done
done

echo "=== 2/5 开环速率扫描 (wrk2, c=100, 15s/档) ==="
for R in 100000 150000 200000 220000 240000 260000 280000 320000; do
  wrk2 -t$T -c100 -d15s -R $R --latency "$URL" > "$OUT/rate_$R.txt" 2>&1
  printf "R=%-7s 实测=%-10s P50=%-9s P99=%-9s\n" "$R" \
    "$(grep 'Requests/sec:' "$OUT/rate_$R.txt" | awk '{print $2}')" \
    "$(grep '^ 50.000%' "$OUT/rate_$R.txt" | awk '{print $2}')" \
    "$(grep '^ 99.000%' "$OUT/rate_$R.txt" | awk '{print $2}')"
done

echo "=== 3/5 单连接流水线深度扫描 (5s x 2 轮) ==="
for K in 1 2 4 8 16 32 64 128 256 512 1024; do
  for r in 1 2; do python3 bench/pipeline_bench.py 1 $K 5; done
done

echo "=== 4/5 连接数 x 深度矩阵 (5s x 2 轮) ==="
for cfg in "8 256" "16 256" "32 256" "16 512" "32 512" "48 512" "64 512" "32 1024" "64 128"; do
  for r in 1 2; do python3 bench/pipeline_bench.py $cfg 5; done
done

echo "=== 5/5 背压验证 (c>=2000 应出现 5xx 增量) ==="
for C in 1000 3000 5000; do
  b=$(curl -s "http://127.0.0.1:${PORT}/stats" | python3 -c "import json,sys; print(json.load(sys.stdin)['err_5xx'])")
  wrk -t$T -c$C -d15s "$URL" > /dev/null
  a=$(curl -s "http://127.0.0.1:${PORT}/stats" | python3 -c "import json,sys; print(json.load(sys.stdin)['err_5xx'])")
  echo "wrk -c$C 15s -> 5xx 增量=$((a-b))"
done

echo "完成。wrk2 原始输出在 $OUT/"
