#!/usr/bin/env bash
# WSS 并发吞吐：并发起 N 条连接，每条各自串行往返，汇总聚合吞吐与延迟分布
#
# 与 d1_throughput 的区别：D1 是**单连接**往返速率（衡量一条连接上的延迟下限），
# 本脚本衡量的是**多连接并发时服务端的聚合处理能力** —— 两者回答的是不同问题，
# 引用时不能互相替代。
#
# 用法: bash scripts/bench_wss_concurrent.sh [conns] [per_conn_msgs] [msg_size]
#   conns          并发连接数（默认 32）
#   per_conn_msgs  每条连接的消息数（默认 2000）
#   msg_size       消息字节数（默认 1024）
# 环境变量: BUILD_DIR(默认 <repo>/build-wss) WSS_HOST WSS_PORT(默认 18990)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-wss}"
CLIENT="$BUILD_DIR/examples/wss_bench_client"
HOST="${WSS_HOST:-localhost}"
PORT="${WSS_PORT:-18990}"
CONNS="${1:-32}"
PER_CONN="${2:-2000}"
SIZE="${3:-1024}"
OUT="$ROOT/results/wss_concurrent_$(date +%Y%m%d_%H%M).txt"
mkdir -p "$ROOT/results"

[ -x "$CLIENT" ] || { echo "缺 $CLIENT（先 cmake -S . -B build-wss -DENABLE_WSS=ON && cmake --build build-wss）"; exit 3; }
# 装置自检：服务没起来就直接判定无效，而不是产出一批全失败的数据
if ! timeout 3 bash -c "echo > /dev/tcp/$HOST/$PORT" 2>/dev/null; then
  echo "[错误] WSS 服务未在 $HOST:$PORT 运行 —— 本次不产出数据"
  echo "       启动: $BUILD_DIR/examples/bench_server --port 18080 --wss-port $PORT --cert <cert> --key <key>"
  exit 3
fi

log() { echo "$@" | tee -a "$OUT"; }
log "# WSS 并发吞吐  $(date '+%F %T')"
log "# 环境: $(nproc) vCPU, $(free -m 2>/dev/null | awk '/[Mm]em|内存/{print $2}') MB"
log "# 参数: 并发连接=$CONNS  每条消息数=$PER_CONN  消息大小=${SIZE}B"
log ""

TMP="$(mktemp -d)"
# warmup=0：并发场景下每条连接各自预热会白烧消息量；改为靠 per_conn_msgs 稀释建连开销
START=$(date +%s.%N)
for i in $(seq 1 "$CONNS"); do
  "$CLIENT" "$HOST" "$PORT" /echo "$PER_CONN" "$SIZE" 1 0 > "$TMP/c$i.txt" 2>&1 &
done
wait
END=$(date +%s.%N)

TOTAL=$((CONNS * PER_CONN))
WALL=$(awk -v s="$START" -v e="$END" 'BEGIN{printf "%.3f", e-s}')
RPS=$(awk -v n="$TOTAL" -v w="$WALL" 'BEGIN{printf "%.0f", n/w}')
# 客户端输出形如 "  完成: 2000/2000 消息"；这里按 "数字/数字" 形状提取分子之和。
# 不能用 -F'[/ ]' 取 $2：分隔符是空格与斜杠的组合，多个连续空格会让字段号漂移（曾因此恒显示 0）
OK=$(grep -h "完成:" "$TMP"/c*.txt 2>/dev/null | \
     awk '{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+\//){split($i,a,"/"); s+=a[1]}} END{print s+0}')
log "总消息数: $TOTAL（成功 $OK）  墙钟: ${WALL}s"
log "聚合吞吐: ${RPS} msg/s"
log ""
# 各连接的延迟分位（客户端各自算，这里汇总成区间）
for q in "p50" "p99"; do
  vals=$(grep -h "延迟 ${q}:" "$TMP"/c*.txt 2>/dev/null | awk '{print $3}' | sort -n)
  if [ -n "$vals" ]; then
    log "延迟 ${q}: 最小 $(echo "$vals" | head -1) ms / 中位 $(echo "$vals" | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}') ms / 最大 $(echo "$vals" | tail -1) ms"
  fi
done
log "（延迟为各连接中位数/尾部的区间；样本分散在各连接，未做跨连接合并分位）"
log ""
log "# 结束 $(date '+%F %T')"
rm -rf "$TMP"
echo "结果写入 $OUT"
