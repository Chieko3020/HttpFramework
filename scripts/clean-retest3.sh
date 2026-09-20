#!/usr/bin/env bash
set -u
W=/tmp/wrk/wrk
DEF=/tmp/hf-fix3-def/examples/bench_server
short() {  # port label
  local port=$1 label=$2
  nohup $DEF --port $port --threads 4 > /tmp/clean-$port.log 2>&1 &
  local want=$!; echo $want > /tmp/clean-$port.pid
  sleep 1.5
  local got; got=$(ss -ltnp 2>/dev/null | grep ":$port " | grep -oP 'pid=\K[0-9]+' | head -1)
  if [ "$got" != "$want" ]; then echo "$label : 端口校验失败 跳过"; kill $want 2>/dev/null; return 1; fi
  $W -t4 -d20s --latency -c1000 -H "Connection: close" "http://127.0.0.1:$port/bench/plaintext" 2>&1 | awk -v L="$label" '/Requests\/sec/{r=$2} /50%/{p=$2} END{print L" : "r" req/s p50="p" (监听者已核验)"}'
  kill $want 2>/dev/null; sleep 1
}
short 19400 "短连接-r1"
short 19401 "短连接-r2"
short 19402 "短连接-r3"
