#!/usr/bin/env bash
# HTTP 性能测量（标准化）：装置自检 + 结果落盘
#
# 为什么要有这个脚本：此前那批复测数据是临时命令跑的，原始输出没入库，
# 于是 README 里的数字既不可追溯、也无法在换机器后重跑对照。本脚本把
# "怎么测"固定下来：启动后先校验监听者身份，再按固定参数跑，输出落到 results/。
#
# 用法: bash scripts/bench_http.sh [quick]
#   quick = 只跑 100/1000 两档 + 内存池单轮（约 2 分钟），默认跑全量（约 8 分钟）
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/build/examples/bench_server"
PORT="${PORT:-18080}"
THREADS="${THREADS:-4}"
WRK="${WRK:-$(command -v wrk || echo /tmp/wrk/wrk)}"
QUICK="${1:-}"
STAMP="$(date +%Y%m%d_%H%M)"
OUT="${ROOT}/results/http_bench_${STAMP}.txt"
mkdir -p "${ROOT}/results"

[ -x "$WRK" ] || { echo "缺 wrk（可设 WRK=/path/to/wrk）"; exit 1; }
[ -x "$BIN" ] || { echo "缺 bench_server：先 cmake --build build"; exit 1; }

log() { echo "$@" | tee -a "$OUT"; }
SRV_PID=""

start_server() {  # $1 = 额外参数（如 --mempool）
  # shellcheck disable=SC2086
  "$BIN" --port "$PORT" --threads "$THREADS" $1 > /tmp/bench_server.log 2>&1 &
  SRV_PID=$!
  for _ in $(seq 1 120); do
    ss -ltn 2>/dev/null | grep -q ":${PORT} " && break
    sleep 0.1
  done
  # 装置自检：进程必须还活着，否则这一批数据作废（启动失败时端口可能被残留进程占着）
  if ! kill -0 "$SRV_PID" 2>/dev/null; then
    log "!! 服务启动失败（参数: $1），本批作废"
    tail -5 /tmp/bench_server.log | tee -a "$OUT"
    return 1
  fi
  log "## 服务 PID=$SRV_PID 参数: ${1:-（默认）}"
  return 0
}

stop_server() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; SRV_PID=""; sleep 1; }

cpu_jiffies() { awk '{print $14+$15}' "/proc/$SRV_PID/stat" 2>/dev/null; }

# $1 = 并发  $2 = 时长  $3 = 标签
run_wrk() {
  local c="$1" d="$2" tag="$3"
  local raw="/tmp/wrk_${c}.txt"
  "$WRK" -t4 -c"$c" -d"$d" --latency "http://127.0.0.1:${PORT}/" > "$raw" 2>&1
  local rps p50 p99 err
  rps=$(awk '/Requests\/sec/{print $2}' "$raw")
  p50=$(awk '/^ *50%/{print $2}' "$raw")
  p99=$(awk '/^ *99%/{print $2}' "$raw")
  err=$(awk '/Socket errors/{print $0}' "$raw" | sed 's/^ *//')
  log "${tag} c=${c} d=${d}: ${rps} req/s  p50=${p50}  p99=${p99}  ${err:-无错误}"
}

log "# HTTP 性能测量  $(date '+%F %T')"
log "# 环境: $(nproc) vCPU, $(free -m | awk '/Mem:/{print $2}') MB, $(uname -r)"
log "# 工具: $("$WRK" --version 2>&1 | head -1)；同机 loopback，服务线程池 ${THREADS}"
log ""

# ---- 1. 并发梯度（长连接）----
log "## 1. 并发梯度（长连接，wrk -t4 -d30s --latency）"
start_server "" || exit 1
if [ "$QUICK" = "quick" ]; then CONCS="100 1000"; else CONCS="100 500 1000 2000 5000"; fi
for c in $CONCS; do run_wrk "$c" 30s "梯度"; done
stop_server

# ---- 2. 每请求 CPU（c=1000）----
log ""
log "## 2. 每请求 CPU（/proc/<pid>/stat 差分 ÷ 请求数，c=1000）"
start_server "" || exit 1
J0=$(cpu_jiffies); T0=$(date +%s.%N)
"$WRK" -t4 -c1000 -d30s "http://127.0.0.1:${PORT}/" > /tmp/wrk_cpu.txt 2>&1
T1=$(date +%s.%N); J1=$(cpu_jiffies)
REQS=$(awk '/requests in/{print $1}' /tmp/wrk_cpu.txt)
HZ=$(getconf CLK_TCK)
awk -v j0="$J0" -v j1="$J1" -v hz="$HZ" -v n="$REQS" \
  'BEGIN{ if (n>0) printf "每请求 CPU: %.1f µs（%d jiffies / %d 请求）\n", (j1-j0)/hz/n*1e6, j1-j0, n; else print "本批无数据" }' | tee -a "$OUT"
stop_server

# ---- 3. 内存池交替对照（默认关闭 vs 开启）----
log ""
log "## 3. 内存池对照（交替执行，各 30s；单轮不足以定论，方向会翻转）"
ROUNDS=3; [ "$QUICK" = "quick" ] && ROUNDS=1
for i in $(seq 1 "$ROUNDS"); do
  start_server "" || break;            run_wrk 1000 30s "无池 第${i}轮"; stop_server
  start_server "--mempool" || break;   run_wrk 1000 30s "开池 第${i}轮"; stop_server
done

# ---- 4. 短连接（波动大，只给区间）----
log ""
log "## 4. 短连接（c=1000，-H 'Connection: close'）——波动大，只作区间参考"
start_server "" || exit 1
for i in 1 2 3; do
  "$WRK" -t4 -c1000 -d30s -H 'Connection: close' "http://127.0.0.1:${PORT}/" > "/tmp/wrk_short_$i.txt" 2>&1
  log "短连接 第${i}次: $(awk '/Requests\/sec/{print $2}' "/tmp/wrk_short_$i.txt") req/s"
done
stop_server

log ""
log "# 结束 $(date '+%F %T')"
log "# 原始 wrk 输出：/tmp/wrk_*.txt（本次读数已汇总在上方）"
echo "结果已写入 ${OUT}"
