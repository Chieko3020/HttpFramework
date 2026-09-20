#!/usr/bin/env bash
set -u
W=/tmp/wrk/wrk
DEF=/tmp/hf-fix3-def/examples/bench_server
run() {  # port label conc url [args...]
  local port=$1 label=$2 conc=$3 url=$4; shift 4
  nohup $DEF --port $port --threads 4 "$@" > /tmp/clean-$port.log 2>&1 &
  local want=$!; echo $want > /tmp/clean-$port.pid
  sleep 1.5
  local got; got=$(ss -ltnp 2>/dev/null | grep ":$port " | grep -oP 'pid=\K[0-9]+' | head -1)
  if [ "$got" != "$want" ]; then echo "$label : 端口校验失败(got=$got want=$want) 跳过"; kill $want 2>/dev/null; return 1; fi
  local r; r=$($W -t4 -d20s --latency -c$conc "http://127.0.0.1:$port$url" 2>&1 | awk '/Requests\/sec/{r=$2} /50%/{p=$2} END{print r" req/s p50="p}')
  echo "$label : $r"
  kill $want 2>/dev/null; sleep 1
}
echo "### 内存池交替对照（端口已校验，20s/轮）"
run 19300 "off-r1" 1000 /bench/plaintext
run 19301 "on-r1 " 1000 /bench/plaintext --mempool
run 19302 "off-r2" 1000 /bench/plaintext
run 19303 "on-r2 " 1000 /bench/plaintext --mempool
run 19304 "off-r3" 1000 /bench/plaintext
run 19305 "on-r3 " 1000 /bench/plaintext --mempool
echo "### 短连接 c=1000（20s）"
run 19306 "短连接-r1" 1000 /bench/plaintext -H "Connection: close"
run 19307 "短连接-r2" 1000 /bench/plaintext -H "Connection: close"
