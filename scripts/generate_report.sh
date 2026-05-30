#!/bin/bash
# generate_report.sh — 解析 wrk 输出，生成双分支对比报告 (纯 Shell)
# 用法: ./scripts/generate_report.sh [results_dir]

set -euo pipefail

RESULTS_DIR="${1:-results}"

# 解析 wrk 输出，提取 Requests/sec
parse_rps() {
    local file="$1"
    if [ -f "$file" ]; then
        grep -oP 'Requests/sec:\s*\K[\d.]+' "$file" | head -1
    else
        echo "-"
    fi
}

# 解析 wrk 输出，提取延迟 p50
parse_latency_p50() {
    local file="$1"
    if [ -f "$file" ]; then
        grep -oP '50%\s+\K[\d.]+\s*\w+' "$file" | head -1
    else
        echo "-"
    fi
}

# 解析 wrk 输出，提取延迟 p99
parse_latency_p99() {
    local file="$1"
    if [ -f "$file" ]; then
        grep -oP '99%\s+\K[\d.]+\s*\w+' "$file" | head -1
    else
        echo "-"
    fi
}

# 解析 wrk 输出，提取 avg 延迟
parse_latency_avg() {
    local file="$1"
    if [ -f "$file" ]; then
        grep -oP 'Latency\s+\K[\d.]+\s*\w+' "$file" | head -1
    else
        echo "-"
    fi
}

# 解析 RSS
parse_rss_max() {
    local file="$1"
    if [ -f "$file" ]; then
        grep VmRSS "$file" | tail -1 | grep -oP '\d+' | awk '{printf "%.1f MB", $1/1024}'
    else
        echo "-"
    fi
}

# 从 b1 文件中解析各线程数的 req/s
parse_thread_rps() {
    local file="$1"
    local thread="$2"
    if [ -f "$file" ]; then
        # 查找 "线程: N" 后紧跟的 Requests/sec
        awk -v t="$thread" '
            /线程: / { current=$0 }
            /Requests/sec:/ {
                if (current ~ "线程: "t"$" || current ~ "线程: "t" ") {
                    match($0, /Requests/sec:\s+([0-9.]+)/, arr)
                    print arr[1]
                    exit
                }
            }
        ' "$file" 2>/dev/null || echo "-"
    else
        echo "-"
    fi
}

# 从 c1 文件中解析各中间件层数的 req/s
parse_middleware_rps() {
    local file="$1"
    local layers="$2"
    if [ -f "$file" ]; then
        awk -v l="$layers" '
            /中间件层数: / { current=$0 }
            /Requests/sec:/ {
                if (current ~ "中间件层数: "l"$" || current ~ "中间件层数: "l" ") {
                    match($0, /Requests/sec:\s+([0-9.]+)/, arr)
                    print arr[1]
                    exit
                }
            }
        ' "$file" 2>/dev/null || echo "-"
    else
        echo "-"
    fi
}

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║   HttpFramework 性能基准测试报告                     ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""
echo "生成时间: $(date)"
echo ""

# ── A1: 纯文本吞吐量 ──────────────────────────────────

echo "━━━ A1: 纯文本吞吐量 ━━━"
echo ""
printf "  %-15s %-15s %-15s\n" "场景" "main(req/s)" "wss(req/s)"
printf "  %-15s %-15s %-15s\n" "─────" "───────────" "───────────"

for conn_label in "plaintext" "plaintext_hc"; do
    if [ "$conn_label" = "plaintext" ]; then
        label="100并发"
    else
        label="1000并发"
    fi
    m=$(parse_rps "$RESULTS_DIR/main/a1_${conn_label}.txt")
    w=$(parse_rps "$RESULTS_DIR/wss/a1_${conn_label}.txt")
    printf "  %-15s %-15s %-15s\n" "$label" "${m:- -}" "${w:- -}"
done
echo ""

# ── A2: JSON ──────────────────────────────────────────

echo "━━━ A2: JSON 响应吞吐量 ━━━"
echo ""
m_rps=$(parse_rps "$RESULTS_DIR/main/a2_json.txt")
w_rps=$(parse_rps "$RESULTS_DIR/wss/a2_json.txt")
printf "  main: %s req/s\n" "${m_rps:- -}"
printf "  wss:  %s req/s\n" "${w_rps:- -}"
echo ""

# ── A3: 延迟 ──────────────────────────────────────────

echo "━━━ A3: 延迟分布 ━━━"
echo ""
printf "  %-8s %-15s %-15s\n" "分位" "main" "wss"
printf "  %-8s %-15s %-15s\n" "───" "────" "───"

for metric in "avg" "p50" "p99"; do
    if [ "$metric" = "avg" ]; then
        m=$(parse_latency_avg "$RESULTS_DIR/main/a3_latency.txt")
        w=$(parse_latency_avg "$RESULTS_DIR/wss/a3_latency.txt")
    else
        m=$(parse_latency_${metric} "$RESULTS_DIR/main/a3_latency.txt")
        w=$(parse_latency_${metric} "$RESULTS_DIR/wss/a3_latency.txt")
    fi
    printf "  %-8s %-15s %-15s\n" "$metric" "${m:- -}" "${w:- -}"
done
echo ""

# ── A5: 内存 ──────────────────────────────────────────

echo "━━━ A5: 内存占用 ━━━"
echo ""
m_rss=$(parse_rss_max "$RESULTS_DIR/main/a5_memory.txt")
w_rss=$(parse_rss_max "$RESULTS_DIR/wss/a5_memory.txt")
printf "  main 峰值 RSS: %s\n" "${m_rss:- -}"
printf "  wss  峰值 RSS: %s\n" "${w_rss:- -}"
echo ""

# ── B1: 线程扩展性 ───────────────────────────────────

echo "━━━ B1: 线程扩展性 ━━━"
echo ""
printf "  %-8s %-15s %-15s\n" "线程数" "main(req/s)" "wss(req/s)"
printf "  %-8s %-15s %-15s\n" "─────" "───────────" "───────────"
for t in 1 2 4 8 16; do
    m=$(parse_thread_rps "$RESULTS_DIR/main/b1_threads.txt" "$t")
    w=$(parse_thread_rps "$RESULTS_DIR/wss/b1_threads.txt" "$t")
    printf "  %-8s %-15s %-15s\n" "$t" "${m:- -}" "${w:- -}"
done
echo ""

# ── B2: 内存池 ───────────────────────────────────────

echo "━━━ B2: 内存池收益 ━━━"
echo ""
printf "  %-10s %-15s %-15s\n" "配置" "main(req/s)" "wss(req/s)"
printf "  %-10s %-15s %-15s\n" "────" "───────────" "───────────"
for mode in "off" "on"; do
    [ "$mode" = "off" ] && label="OFF" || label="ON"
    m=$(parse_rps "$RESULTS_DIR/main/b2_mempool_${mode}.txt")
    w=$(parse_rps "$RESULTS_DIR/wss/b2_mempool_${mode}.txt")
    printf "  %-10s %-15s %-15s\n" "$label" "${m:- -}" "${w:- -}"
done
echo ""

# ── B3: 路由扩展性 ───────────────────────────────────

echo "━━━ B3: 路由扩展性 ━━━"
echo ""
m_file="$RESULTS_DIR/main/b3_routes.txt"
w_file="$RESULTS_DIR/wss/b3_routes.txt"
printf "  %-10s %-15s %-15s\n" "路由数" "main(req/s)" "wss(req/s)"
printf "  %-10s %-15s %-15s\n" "─────" "───────────" "───────────"

if [ -f "$m_file" ] || [ -f "$w_file" ]; then
    for n in 10 100 500 1000; do
        m=$(awk -v t="$n" '/路由数: /{current=$0} /Requests/sec:/{if(current~"路由数: "t"$"||current~"路由数: "t" "){match($0,/Requests/sec:\s+([0-9.]+)/,arr);print arr[1];exit}}' "$m_file" 2>/dev/null || echo "-")
        w=$(awk -v t="$n" '/路由数: /{current=$0} /Requests/sec:/{if(current~"路由数: "t"$"||current~"路由数: "t" "){match($0,/Requests/sec:\s+([0-9.]+)/,arr);print arr[1];exit}}' "$w_file" 2>/dev/null || echo "-")
        printf "  %-10s %-15s %-15s\n" "$n" "${m:- -}" "${w:- -}"
    done
else
    echo "  (未运行)"
fi
echo ""

# ── C1: 中间件开销 ───────────────────────────────────

echo "━━━ C1: 中间件开销 ━━━"
echo ""
printf "  %-8s %-15s %-15s\n" "层数" "main(req/s)" "wss(req/s)"
printf "  %-8s %-15s %-15s\n" "───" "───────────" "───────────"
for layers in 0 1 5 10; do
    m=$(parse_middleware_rps "$RESULTS_DIR/main/c1_middleware.txt" "$layers")
    w=$(parse_middleware_rps "$RESULTS_DIR/wss/c1_middleware.txt" "$layers")
    printf "  %-8s %-15s %-15s\n" "$layers" "${m:- -}" "${w:- -}"
done
echo ""

# ── C2: 会话 ──────────────────────────────────────────

echo "━━━ C2: 会话开销 ━━━"
echo ""
printf "  %-10s %-15s %-15s\n" "配置" "main(req/s)" "wss(req/s)"
printf "  %-10s %-15s %-15s\n" "────" "───────────" "───────────"
for mode in "off" "on"; do
    [ "$mode" = "off" ] && label="OFF" || label="ON"
    m=$(parse_rps "$RESULTS_DIR/main/c2_session_${mode}.txt")
    w=$(parse_rps "$RESULTS_DIR/wss/c2_session_${mode}.txt")
    printf "  %-10s %-15s %-15s\n" "$label" "${m:- -}" "${w:- -}"
done
echo ""

# ── WSS 专属 (如果有结果) ──────────────────────────────

echo "━━━ WSS 专属指标 ━━━"
echo ""
wss_dir="$RESULTS_DIR/wss"

# D1: 吞吐量
for labelsize in "256B" "1KB" "16KB"; do
    f="${wss_dir}/d1_throughput_${labelsize}.txt"
    if [ -f "$f" ]; then
        msg_sec=$(grep "消息/秒:" "$f" | head -1 | awk '{print $NF}')
        echo "  WSS 消息吞吐量 (${labelsize}): ${msg_sec:- -} msg/s"
    fi
done

# D2: 握手
f="${wss_dir}/d2_handshake.txt"
if [ -f "$f" ]; then
    hps=$(grep "握手/秒:" "$f" | head -1 | awk '{print $NF}')
    success=$(grep "成功:" "$f" | head -1 | awk '{print $NF}' | tr -d ',')
    echo "  TLS 握手: ${success:- -} 成功, ${hps:- -} 握手/s"
fi

# D3: 并发
f="${wss_dir}/d3_maxconn.txt"
if [ -f "$f" ]; then
    max=$(grep "成功建立:" "$f" | head -1 | awk '{print $NF}')
    echo "  最大并发 WSS 连接: ${max:- -}"
fi

# D5: 文件传输
for size in "64kb" "256kb"; do
    f="${wss_dir}/d5_filetransfer_${size}.txt"
    if [ -f "$f" ]; then
        tput=$(grep "吞吐量:" "$f" | head -1 | awk '{print $NF}')
        echo "  文件传输 (${size}): ${tput:- -}"
    fi
done

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║   报告生成完成                                      ║"
echo "╚══════════════════════════════════════════════════╝"
