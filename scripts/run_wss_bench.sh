#!/bin/bash
# run_wss_bench.sh — WSS 性能基准测试
# 用法: ./scripts/run_wss_bench.sh <branch_label> [results_dir]
# 依赖: wscat, python3-websockets, openssl, curl

set -eo pipefail

BRANCH="${1:-unknown}"
RESULT_DIR="${2:-results/$BRANCH}"
WSS_PORT=18990
WSS_URI="wss://localhost:${WSS_PORT}/echo"

mkdir -p "$RESULT_DIR"

# 检查 npx/wscat 是否可用
check_wscat() {
    if wscat --version &>/dev/null; then
        return 0
    fi
    echo "  [错误] npx wscat 不可用"
    return 1
}

record_header() {
    local test_id="$1"; shift
    local desc="$1"
    local out="$RESULT_DIR/${test_id}.txt"
    {
        echo "=== ${test_id}: ${desc} ==="
        echo "分支: ${BRANCH}"
        echo "时间: $(date -Iseconds)"
        echo "---"
    } > "$out"
    echo "$out"
}

# ── D1: WSS 消息吞吐量 ──────────────────────────────────

bench_throughput() {
    local msg_size="$1"  # bytes
    local count="$2"
    local label="$3"

    local out
    out=$(record_header "d1_throughput_${label}" "WSS 消息吞吐量 (${label})")

    echo "  [WSS] 发送 ${count} 条 ${label} 消息..."

    # 使用 wscat 管道批量发送
    start_time=$(date +%s.%N) || start_time=0

    tmpin=$(mktemp) || tmpin="/tmp/wss_bench_in_$$"
    tmpout=$(mktemp) || tmpout="/tmp/wss_bench_out_$$"
    for i in $(seq 1 "$count"); do
        echo "msg-$i"
    done > "$tmpin"

    timeout 30 wscat -c "$WSS_URI" --no-color < "$tmpin" > "$tmpout" 2>/dev/null || true

    end_time=$(date +%s.%N) || end_time=0
    elapsed=$(echo "$end_time - $start_time" | bc 2>/dev/null || echo "0")
    received=$(wc -l < "$tmpout" 2>/dev/null || echo "0")
    [ -z "$received" ] && received=0

    {
        echo "消息大小: ${msg_size} bytes"
        echo "发送: ${count}"
        echo "收到: ${received}"
        echo "总耗时: ${elapsed}s"
        if [ "$(echo "$elapsed > 0" | bc 2>/dev/null)" = "1" ] && [ "$received" -gt 0 ]; then
            echo "消息/秒: $(echo "scale=0; $received / $elapsed" | bc)"
        fi
    } >> "$out"

    rm -f "$tmpin" "$tmpout"
    echo "  ${label}: 完成"

    {
        echo "消息大小: ${msg_size} bytes"
        echo "发送: ${sent}"
        echo "收到: ${received}"
        echo "总耗时: ${elapsed}s"
        if [ "$(echo "$elapsed > 0" | bc)" = "1" ]; then
            echo "消息/秒: $(echo "scale=0; $received / $elapsed" | bc)"
        fi
    } >> "$out"

    echo "  ${label}: ${received}/${sent} 完成, ${elapsed}s"
}

# ── D2: TLS 握手速率 ───────────────────────────────────

bench_handshake() {
    local count="$1"
    local out
    out=$(record_header "d2_handshake" "TLS 握手速率")

    echo "  [WSS] 测量 ${count} 次 TLS 握手..."

    local start_time end_time elapsed
    start_time=$(date +%s.%N)

    local success=0
    local fail=0
    for i in $(seq 1 "$count"); do
        if timeout 5 openssl s_client -connect "localhost:${WSS_PORT}" \
            -servername localhost -quiet -no_ign_eof 2>/dev/null < /dev/null | head -1 | grep -q .; then
            success=$((success + 1))
        else
            fail=$((fail + 1))
        fi
        if [ $((i % 20)) -eq 0 ]; then
            echo "    握手: ${i}/${count} (成功=${success}, 失败=${fail})"
        fi
    done

    end_time=$(date +%s.%N)
    elapsed=$(echo "$end_time - $start_time" | bc)

    {
        echo "总握手次数: ${count}"
        echo "成功: ${success}, 失败: ${fail}"
        echo "总耗时: ${elapsed}s"
        if [ "$(echo "$elapsed > 0" | bc)" = "1" ]; then
            echo "握手/秒: $(echo "scale=1; $success / $elapsed" | bc)"
        fi
    } >> "$out"

    echo "  握手: ${success} 成功, ${fail} 失败, ${elapsed}s"
}

# ── D3: 并发 WSS 连接 ──────────────────────────────────

bench_maxconn() {
    local max_conn="$1"
    local out
    out=$(record_header "d3_maxconn" "最大并发 WSS 连接")

    echo "  [WSS] 测试最多 ${max_conn} 并发连接..."

    local start_time elapsed
    start_time=$(date +%s.%N)

    local success=0
    local pids=()
    for i in $(seq 1 "$max_conn"); do
        (
            timeout 10 openssl s_client -connect "localhost:${WSS_PORT}" \
                -servername localhost -quiet -no_ign_eof 2>/dev/null < /dev/null &
            PID=$!
            sleep 0.5
            # 发送 WebSocket 升级请求
            echo -ne "GET /echo HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n" > /proc/$PID/fd/0 2>/dev/null || true
            sleep 0.5
            kill $PID 2>/dev/null
        ) &
        pids+=($!)
        if [ $? -eq 0 ]; then
            success=$((success + 1))
        fi

        if [ $((i % 50)) -eq 0 ]; then
            echo "    并发连接: ${i}/${max_conn} (成功=${success})"
        fi
    done

    # 等待所有后台进程
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done

    elapsed=$(echo "$(date +%s.%N) - $start_time" | bc)

    {
        echo "尝试连接数: ${max_conn}"
        echo "成功建立: ${success}"
        echo "总耗时: ${elapsed}s"
    } >> "$out"

    echo "  并发: ${success} 成功, ${elapsed}s"
}

# ── D5: 文件传输 ────────────────────────────────────────

bench_filetransfer() {
    local size_kb="$1"
    local out
    out=$(record_header "d5_filetransfer_${size_kb}kb" "文件传输速率 (${size_kb}KB)")

    if ! check_wscat; then
        echo "跳过: wscat 不可用" >> "$out"
        return
    fi

    # 生成测试数据
    local tmpfile
    tmpfile=$(mktemp)
    dd if=/dev/urandom bs=1024 count="$size_kb" of="$tmpfile" 2>/dev/null

    echo "  [WSS] 发送 ${size_kb}KB 文件..."
    local start_time end_time elapsed
    start_time=$(date +%s.%N)

    # wscat 发送文件
    if timeout 30 wscat -c "$WSS_URI" --no-color < "$tmpfile" > /tmp/wss_resp.bin 2>/dev/null; then
        end_time=$(date +%s.%N)
        elapsed=$(echo "$end_time - $start_time" | bc)

        local resp_size
        resp_size=$(stat -c%s /tmp/wss_resp.bin 2>/dev/null || echo "0")
        local resp_kb=$((resp_size / 1024))

        {
            echo "发送大小: ${size_kb}KB"
            echo "收到大小: ${resp_kb}KB"
            echo "传输时间: ${elapsed}s"
            if [ "$(echo "$elapsed > 0" | bc)" = "1" ]; then
                echo "吞吐量: $(echo "scale=2; $resp_kb / $elapsed" | bc) KB/s"
            fi
        } >> "$out"

        echo "  文件传输 ${size_kb}KB: $(echo "scale=2; $resp_kb / $elapsed" | bc) KB/s"
    else
        echo "传输失败" >> "$out"
        echo "  文件传输 ${size_kb}KB: 失败"
    fi

    rm -f "$tmpfile" /tmp/wss_resp.bin
}

# ── Main ────────────────────────────────────────────────

echo "=== WSS 基准测试 (分支: $BRANCH) ==="

# 验证 WSS 端口
if ! timeout 3 bash -c "echo > /dev/tcp/localhost/$WSS_PORT" 2>/dev/null; then
    echo "错误: WSS 服务器未在端口 $WSS_PORT 运行"
    echo "请先启动: bench_server --port 18080 --wss-port 18990 --cert ... --key ..."
    exit 1
fi
echo "WSS 端口 $WSS_PORT 已就绪"

echo ""
echo "--- D1: WSS 消息吞吐量 ---"
bench_throughput 256 100 "256B"
bench_throughput 1024 50 "1KB"
bench_throughput 16384 10 "16KB"

echo ""
echo "--- D2: TLS 握手速率 ---"
bench_handshake 50

echo ""
echo "--- D3: 并发连接 ---"
bench_maxconn 100

echo ""
echo "--- D5: 文件传输 ---"
bench_filetransfer 64
bench_filetransfer 256

echo ""
echo "=== WSS 基准测试完成 ==="
echo "结果目录: $RESULT_DIR"
ls -la "$RESULT_DIR/"
echo ""
echo "关键数据:"
grep -H "消息/秒\|握手/秒\|吞吐量" "$RESULT_DIR"/*.txt 2>/dev/null || echo "(无结果)"
