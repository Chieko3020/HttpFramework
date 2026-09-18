#!/bin/bash
# run_http_bench.sh — HTTP 基准测试全部指标
# 用法: ./scripts/run_http_bench.sh <branch_label> [results_dir]
# 依赖: wrk, bench_server

set -euo pipefail

BRANCH="${1:-unknown}"
RESULT_DIR="${2:-results/$BRANCH}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_SERVER="${BENCH_SERVER:-$PROJECT_DIR/build/examples/bench_server}"
PORT=18980
# 压测端线程数：小核数机器建议与核数相当，避免压测端与被测服务抢 CPU
WRK_THREADS="${WRK_THREADS:-2}"

mkdir -p "$RESULT_DIR"

# 清理函数：先 SIGTERM 等待退出，超时再 SIGKILL，避免端口/进程残留影响下一项
cleanup() {
    if [ -n "${SERVER_PID:-}" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        for _ in $(seq 1 25); do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.2
        done
        kill -KILL "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}
trap cleanup EXIT

# 启动服务器并轮询等待就绪（最多 10s，避免固定 sleep 在高负载下不足而误报"启动失败"）
start_server() {
    local desc="$1"; shift
    echo "  [启动] $desc"
    local slog="/tmp/hf-test/server_${desc//[^A-Za-z0-9]/_}.log"
    "$BENCH_SERVER" --port "$PORT" "$@" > "$slog" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 50); do
        if curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/bench/plaintext" 2>/dev/null | grep -q 200; then
            return 0
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "  [错误] 服务器进程已退出，日志尾部（$slog）:"
            tail -5 "$slog" 2>/dev/null
            exit 1
        fi
        sleep 0.2
    done
    echo "  [错误] 服务器 10s 内未就绪，日志尾部（$slog）:"
    tail -5 "$slog" 2>/dev/null
    exit 1
}

# 记录结果文件头
record() {
    local test_id="$1"; shift
    local desc="$1"; shift
    local out="$RESULT_DIR/${test_id}.txt"
    echo "=== $test_id: $desc ===" | tee "$out"
    echo "分支: $BRANCH" | tee -a "$out"
    echo "时间: $(date -Iseconds)" | tee -a "$out"
    echo "环境: $(nproc) vCPU / $(awk '/MemTotal/{printf "%.0f", $2/1024}' /proc/meminfo) MB / $(uname -sr)" | tee -a "$out"
    echo "工具: $(wrk --version 2>&1 | head -1)" | tee -a "$out"
    echo "说明: 压测端与被测服务同机（环回），服务线程数见各项 --threads，wrk 线程数 -t$WRK_THREADS" | tee -a "$out"
    echo "---" | tee -a "$out"
}

# ─── A1: 纯文本吞吐量 ──────────────────────────────────

echo "=== A1: 纯文本吞吐量 ==="
start_server "默认配置 (4 线程)" --threads 4
record "a1_plaintext" "纯文本吞吐量 - 2 连接"
wrk -t"$WRK_THREADS" -c100 -d30s "http://localhost:$PORT/bench/plaintext" | tee -a "$RESULT_DIR/a1_plaintext.txt"
cleanup

start_server "1000 并发" --threads 4
record "a1_plaintext_hc" "纯文本吞吐量 - 1000 并发"
wrk -t"$WRK_THREADS" -c1000 -d30s "http://localhost:$PORT/bench/plaintext" | tee -a "$RESULT_DIR/a1_plaintext_hc.txt"
cleanup

# ─── A2: JSON 吞吐量 ──────────────────────────────────

echo "=== A2: JSON 吞吐量 ==="
start_server "JSON 响应" --threads 4
record "a2_json" "JSON 响应吞吐量"
wrk -t"$WRK_THREADS" -c100 -d30s "http://localhost:$PORT/bench/json" | tee -a "$RESULT_DIR/a2_json.txt"
cleanup

# ─── A3: 延迟分布 ─────────────────────────────────────

echo "=== A3: 延迟分布 ==="
start_server "延迟测试" --threads 4
record "a3_latency" "延迟分布 (带详细分位)"
wrk -t"$WRK_THREADS" -c100 -d30s --latency "http://localhost:$PORT/bench/json" | tee -a "$RESULT_DIR/a3_latency.txt"
cleanup

# ─── A4: 最大并发连接 ─────────────────────────────────

echo "=== A4: 最大并发连接 ==="
start_server "最大连接数测试" --threads 8
record "a4_maxconn" "逐步提高并发找上限"
for c in 100 500 1000 2000 5000; do
    echo "  --- 并发: $c ---" | tee -a "$RESULT_DIR/a4_maxconn.txt"
    wrk -t"$WRK_THREADS" -c"$c" -d15s --timeout 5s "http://localhost:$PORT/bench/plaintext" | tee -a "$RESULT_DIR/a4_maxconn.txt" || true
done
cleanup

# ─── A5: 内存占用 ─────────────────────────────────────

echo "=== A5: 内存占用 ==="
start_server "内存测试" --threads 4
record "a5_memory" "内存占用 (RSS) 在持续负载下"
# 取样初始 RSS
PID=$(pgrep -f "bench_server.*--port $PORT" | head -1)
if [ -n "$PID" ]; then
    echo "初始 RSS:" | tee -a "$RESULT_DIR/a5_memory.txt"
    grep VmRSS /proc/"$PID"/status | tee -a "$RESULT_DIR/a5_memory.txt"
fi
# 持续压测 60 秒
wrk -t"$WRK_THREADS" -c500 -d60s "http://localhost:$PORT/bench/plaintext" > /dev/null 2>&1 &
WRK_PID=$!
sleep 30
if [ -n "$PID" ]; then
    echo "负载中 RSS (30s):" | tee -a "$RESULT_DIR/a5_memory.txt"
    grep VmRSS /proc/"$PID"/status | tee -a "$RESULT_DIR/a5_memory.txt"
fi
sleep 30
if [ -n "$PID" ]; then
    echo "负载中 RSS (60s):" | tee -a "$RESULT_DIR/a5_memory.txt"
    grep VmRSS /proc/"$PID"/status | tee -a "$RESULT_DIR/a5_memory.txt"
fi
wait $WRK_PID 2>/dev/null || true
# 负载后 RSS
sleep 5
if [ -n "$PID" ]; then
    echo "负载后 RSS (空闲):" | tee -a "$RESULT_DIR/a5_memory.txt"
    grep VmRSS /proc/"$PID"/status | tee -a "$RESULT_DIR/a5_memory.txt"
fi
cleanup

# ─── A6: 每请求 CPU 成本 ───────────────────────────────

echo "=== A6: 每请求 CPU 成本 ==="
start_server "CPU 成本测试" --threads 4
record "a6_cpu_cost" "每请求 CPU 时间 (utime+stime / 请求数)"
PID=$(pgrep -f "bench_server.*--port $PORT" | head -1)
if [ -n "$PID" ]; then
    read_cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
    TICKS0=$(read_cpu_ticks "$PID")
    WRK_OUT=$(wrk -t"$WRK_THREADS" -c500 -d30s "http://localhost:$PORT/bench/plaintext")
    TICKS1=$(read_cpu_ticks "$PID")
    REQS=$(echo "$WRK_OUT" | awk '/requests in/{print $1}')
    HZ=$(getconf CLK_TCK)
    echo "$WRK_OUT" | tee -a "$RESULT_DIR/a6_cpu_cost.txt"
    if [ -n "${REQS:-}" ] && [ "$REQS" -gt 0 ]; then
        CPU_US=$(echo "scale=2; ($TICKS1-$TICKS0)*1000000/$HZ/$REQS" | bc)
        echo "CPU ticks: $TICKS0 -> $TICKS1 (CLK_TCK=$HZ)" | tee -a "$RESULT_DIR/a6_cpu_cost.txt"
        echo "总请求: $REQS" | tee -a "$RESULT_DIR/a6_cpu_cost.txt"
        echo "每请求 CPU 时间: ${CPU_US} us" | tee -a "$RESULT_DIR/a6_cpu_cost.txt"
    fi
fi
cleanup

# ─── B1: 线程扩展性 ───────────────────────────────────

echo "=== B1: 线程扩展性 ==="
record "b1_threads" "不同线程数的吞吐量变化"
for t in 1 2 4 8 16; do
    echo "  --- 线程: $t ---" | tee -a "$RESULT_DIR/b1_threads.txt"
    start_server "线程扩展性 (t=$t)" --threads "$t"
    wrk -t"$WRK_THREADS" -c100 -d15s "http://localhost:$PORT/bench/plaintext" | tee -a "$RESULT_DIR/b1_threads.txt"
    cleanup
done

# ─── B2: 内存池收益 ───────────────────────────────────

echo "=== B2: 内存池收益 ==="
start_server "禁用内存池" --threads 4
record "b2_mempool_off" "内存池 OFF 时的吞吐量"
wrk -t"$WRK_THREADS" -c500 -d30s "http://localhost:$PORT/bench/plaintext" | tee -a "$RESULT_DIR/b2_mempool_off.txt"
cleanup

start_server "启用内存池" --threads 4 --mempool
record "b2_mempool_on" "内存池 ON 时的吞吐量"
wrk -t"$WRK_THREADS" -c500 -d30s "http://localhost:$PORT/bench/plaintext" | tee -a "$RESULT_DIR/b2_mempool_on.txt"
cleanup

# ─── B3: 路由扩展性 ───────────────────────────────────

echo "=== B3: 路由扩展性 ==="
record "b3_routes" "不同路由数量的吞吐量退化"
for n in 10 100 500 1000; do
    echo "  --- 路由数: $n ---" | tee -a "$RESULT_DIR/b3_routes.txt"
    start_server "路由扩展性 (n=$n)" --threads 4 --routes "$n"
    wrk -t"$WRK_THREADS" -c100 -d15s "http://localhost:$PORT/bench/plaintext" | tee -a "$RESULT_DIR/b3_routes.txt"
    cleanup
done

# ─── C1: 中间件开销 ───────────────────────────────────

echo "=== C1: 中间件开销 ==="
record "c1_middleware" "不同中间件层数的吞吐量变化"
for m in 0 1 5 10; do
    echo "  --- 中间件层数: $m ---" | tee -a "$RESULT_DIR/c1_middleware.txt"
    start_server "中间件开销 (m=$m)" --threads 4 --middleware "$m"
    wrk -t"$WRK_THREADS" -c100 -d15s "http://localhost:$PORT/bench/plaintext" | tee -a "$RESULT_DIR/c1_middleware.txt"
    cleanup
done

# ─── C2: 会话开销 ─────────────────────────────────────

echo "=== C2: 会话开销 ==="
start_server "无 Session" --threads 4
record "c2_session_off" "Session OFF 时的吞吐量"
wrk -t"$WRK_THREADS" -c100 -d30s "http://localhost:$PORT/bench/plaintext" | tee -a "$RESULT_DIR/c2_session_off.txt"
cleanup

start_server "启用 Session" --threads 4 --session
record "c2_session_on" "Session ON 时的吞吐量"
wrk -t"$WRK_THREADS" -c100 -d30s --header "Cookie: session_id=bench-test-session" "http://localhost:$PORT/bench/session" | tee -a "$RESULT_DIR/c2_session_on.txt"
cleanup

# ─── 汇总 ──────────────────────────────────────────────

echo ""
echo "=== HTTP 基准测试完成 ==="
echo "结果目录: $RESULT_DIR"
echo "文件列表:"
ls -la "$RESULT_DIR/"
echo ""
echo "关键指标提取:"
grep -H "Requests/sec" "$RESULT_DIR"/*.txt 2>/dev/null || echo "(无结果)"
