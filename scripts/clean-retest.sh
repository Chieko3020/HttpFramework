#!/usr/bin/env bash
set -u
W=/tmp/wrk/wrk
BASE=/tmp/hf-base-build/examples/bench_server
DEF=/tmp/hf-fix3-def/examples/bench_server
run() {  # port bin label conc url [server args...]
  local port=$1 bin=$2 label=$3 conc=$4 url=$5; shift 5
  nohup $bin --port $port --threads 4 "$@" > /tmp/clean-$port.log 2>&1 &
  local want=$!
  echo $want > /tmp/clean-$port.pid
  sleep 1.5
  local got
  got=$(ss -ltnp 2>/dev/null | grep ":$port " | grep -oP 'pid=\K[0-9]+' | head -1)
  if [ "$got" != "$want" ]; then
    echo "--- $label : !! 端口 $port 的监听者 PID=$got ≠ 期望 $want —— 结果无效，跳过"
    kill "$want" 2>/dev/null; return 1
  fi
  printf "--- %s (PID %s 已核验)  " "$label" "$got"
  $W -t4 -d15s --latency -c$conc "http://127.0.0.1:$port$url" 2>&1 | awk '/Requests\/sec/{r=$2} /50%/{p=$2} END{printf "req/s=%s p50=%s\n", r, p}'
  kill "$want" 2>/dev/null; sleep 1
}
echo "### 中间件 50 层"
run 19200 $BASE "修复前 @c=20"   20   /bench/plaintext --middleware 50
run 19201 $DEF  "修复后 @c=20"   20   /bench/plaintext --middleware 50
run 19202 $BASE "修复前 @c=1000" 1000 /bench/plaintext --middleware 50
run 19203 $DEF  "修复后 @c=1000" 1000 /bench/plaintext --middleware 50
echo "### 路由 500 条 · 命中末条"
run 19204 $BASE "修复前 @c=20"   20   /bench/routes/499 --routes 500
run 19205 $DEF  "修复后 @c=20"   20   /bench/routes/499 --routes 500
echo "### 路由 500 条 · 命中靠前（对照）"
run 19206 $BASE "修复前 @c=20"   20   /bench/plaintext --routes 500
run 19207 $DEF  "修复后 @c=20"   20   /bench/plaintext --routes 500
echo "=== CLEAN-RETEST-DONE ==="
