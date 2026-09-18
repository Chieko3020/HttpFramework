#!/bin/bash
# run_wss_bench.sh — WSS 性能基准测试
# 用法: ./scripts/run_wss_bench.sh <branch_label> [results_dir]
# 依赖: openssl (D2/D3), C++ 基准客户端 $BUILD_DIR/examples/wss_bench_client (D1),
#       wscat (D5，未安装则明确跳过)
#
# 可用环境变量覆盖: BUILD_DIR (默认 <repo>/build), WSS_PORT (默认 18990),
#                   D1_COUNT (每档消息数，默认 1000), D2_COUNT (默认 50), D3_MAXCONN (默认 100)

set -eo pipefail

BRANCH="${1:-unknown}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# 公共函数：结果目录映射 + 结果文件环境锚定（与 run_http_bench / bench_all 共用一份）
. "$PROJECT_DIR/scripts/bench_env.sh"

RESULT_DIR="${2:-$(bench_result_dir "$BRANCH")}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build}"
WSS_PORT="${WSS_PORT:-18990}"
# D5 用到的 WSS 端点（此前从未赋值，导致 wscat 拿到空 URI）
WSS_URI="wss://localhost:${WSS_PORT}/echo"
D1_COUNT="${D1_COUNT:-1000}"
D2_COUNT="${D2_COUNT:-50}"
D3_MAXCONN="${D3_MAXCONN:-100}"
# D3 单连接探测：保持 stdin 打开 PROBE_HOLD 秒，等升级响应（硬上限 PROBE_TIMEOUT）
# 说明：并发握手总耗时 ≈ 连接数 × 单次握手耗时（本机实测单次 ~0.1s），
#       保持时间太短会让客户端在服务端答复前就断开，被误判为失败。
PROBE_HOLD="${PROBE_HOLD:-8}"
PROBE_TIMEOUT="${PROBE_TIMEOUT:-$((PROBE_HOLD + 5))}"

mkdir -p "$RESULT_DIR"

record_header() {
    local test_id="$1"; shift
    local desc="$1"
    local out="$RESULT_DIR/${test_id}.txt"
    {
        echo "=== ${test_id}: ${desc} ==="
        echo "分支: ${BRANCH}"
        echo "时间: $(date -Iseconds)"
        bench_env_anchor
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

    local client="$BUILD_DIR/examples/wss_bench_client"
    if [ ! -x "$client" ]; then
        bench_write_skip "$out" "wss_bench_client 未编译（$client 不存在）" \
            "cmake -S . -B $BUILD_DIR -DENABLE_WSS=ON -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR -j2"
        echo "  [跳过] ${label}: 客户端未编译（$client）"
        return
    fi

    echo "  [WSS] 发送 ${count} 条 ${label} 消息 (C++ 客户端)..."
    "$client" localhost "$WSS_PORT" /echo "$count" "$msg_size" >> "$out" 2>&1
    echo "  ${label}: 完成"
}

# ── D2: TLS 握手速率 ───────────────────────────────────

bench_handshake() {
    local count="$1"
    local out
    out=$(record_header "d2_handshake" "TLS 握手速率")

    echo "  [WSS] 测量 ${count} 次 TLS 握手..."

    # 判据：openssl s_client -brief 输出里的 "CONNECTION ESTABLISHED"（该输出走 stderr）
    # 旧判据 "-quiet 的 stdout 是否为空" 恒为空，所以旧结果文件里成功数恒为 0。
    local start_time end_time elapsed
    start_time=$(date +%s.%N)

    local success=0
    local fail=0
    local unknown=0
    local protos
    protos=$(mktemp)
    for i in $(seq 1 "$count"); do
        local hs_out hs_proto
        hs_out=$(timeout 5 openssl s_client -connect "localhost:${WSS_PORT}" \
            -servername localhost -brief -no_ign_eof </dev/null 2>&1) || true
        if printf '%s' "$hs_out" | grep -q 'CONNECTION ESTABLISHED'; then
            success=$((success + 1))
            hs_proto=$(printf '%s' "$hs_out" | grep -oP 'Protocol version:\s*\K\S+' | head -1 || true)
            if [ -n "$hs_proto" ]; then echo "${hs_proto}" >> "$protos"; fi
        elif [ -z "$hs_out" ]; then
            unknown=$((unknown + 1))
        else
            fail=$((fail + 1))
        fi
        if [ $((i % 20)) -eq 0 ]; then
            echo "    握手: ${i}/${count} (成功=${success}, 失败=${fail}, 未知=${unknown})"
        fi
    done

    end_time=$(date +%s.%N)
    elapsed=$(awk -v a="$start_time" -v b="$end_time" 'BEGIN{printf "%.6f", b-a}')

    {
        echo "总握手次数: ${count}"
        echo "成功: ${success}, 失败: ${fail}, 未能判定(unknown): ${unknown}"
        echo "判据: openssl s_client -brief 输出含 'CONNECTION ESTABLISHED'"
        if [ -s "$protos" ]; then
            echo "协商协议版本分布: $(sort "$protos" | uniq -c | awk '{printf "%s×%s ", $2, $1}')"
        fi
        echo "总耗时: ${elapsed}s"
        if awk -v e="$elapsed" 'BEGIN{exit !(e > 0)}'; then
            echo "握手/秒: $(awk -v s="$success" -v e="$elapsed" 'BEGIN{printf "%.1f", s/e}')"
        fi
    } >> "$out"
    rm -f "$protos"

    echo "  握手: ${success} 成功, ${fail} 失败, ${unknown} 未知, ${elapsed}s"
}

# ── D3: 并发 WSS 连接 ──────────────────────────────────

# 单次探测：TLS 建连 + 发 WebSocket 升级请求，按响应首行判定结果。
# 输出 ok / fail / unknown（无法判定时记 unknown，绝不计成功）
probe_ws_upgrade() {
    local resp
    resp=$( { printf '%b' "$WS_UPGRADE_REQ"; sleep "$PROBE_HOLD"; } | \
        timeout "$PROBE_TIMEOUT" openssl s_client -connect "localhost:${WSS_PORT}" \
            -servername localhost -quiet -no_ign_eof 2>/dev/null | head -c 4096 ) || true
    if [ -z "$resp" ]; then
        printf 'unknown\n'
    elif printf '%s' "$resp" | grep -qE '^HTTP/1\.[01] 101'; then
        printf 'ok\n'
    else
        printf 'fail\n'
    fi
}

bench_maxconn() {
    local max_conn="$1"
    local out
    out=$(record_header "d3_maxconn" "最大并发 WSS 连接")

    echo "  [WSS] 测试最多 ${max_conn} 并发连接..."

    local start_time elapsed
    start_time=$(date +%s.%N)

    local tmpdir
    tmpdir=$(mktemp -d)
    local WS_UPGRADE_REQ='GET /echo HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n'

    local pids=()
    local i
    for i in $(seq 1 "$max_conn"); do
        (
            probe_ws_upgrade > "$tmpdir/$i" 2>/dev/null
        ) &
        pids+=($!)
        # 提示：这里不再用 $? 判断（那是数组赋值的退出码，恒为 0）；
        # 成功与否由每个连接自己的探测结果文件判定。
        if [ $((i % 50)) -eq 0 ]; then
            echo "    并发连接: 已发起 ${i}/${max_conn}"
        fi
    done

    # 等待所有后台探测结束
    local pid
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done

    elapsed=$(awk -v a="$start_time" -v b="$(date +%s.%N)" 'BEGIN{printf "%.6f", b-a}')

    local success fail unknown
    success=$(cat "$tmpdir"/* 2>/dev/null | grep -c '^ok$' || true)
    fail=$(cat "$tmpdir"/* 2>/dev/null | grep -c '^fail$' || true)
    unknown=$(cat "$tmpdir"/* 2>/dev/null | grep -c '^unknown$' || true)

    {
        echo "尝试连接数: ${max_conn}"
        echo "成功建立(收到 101 Switching Protocols): ${success}"
        echo "失败(有响应但非 101): ${fail}"
        echo "未能判定(无响应/超时): ${unknown}"
        echo "判据: 每个并发连接的升级响应首行匹配 ^HTTP/1.[01] 101；未能判定不计入成功"
        echo "探测参数: 每连接保持 stdin ${PROBE_HOLD}s（并发 TLS 握手在服务端串行排队，保持过短会误判失败）"
        echo "总耗时: ${elapsed}s"
    } >> "$out"
    rm -rf "$tmpdir"

    echo "  并发: ${success} 成功, ${fail} 失败, ${unknown} 未知, ${elapsed}s"
}

# ── D5: 文件传输 ────────────────────────────────────────

bench_filetransfer() {
    local size_kb="$1"
    local out
    out=$(record_header "d5_filetransfer_${size_kb}kb" "文件传输速率 (${size_kb}KB)")

    if ! bench_has_tool wscat; then
        bench_write_skip "$out" "wscat 不可用（D5 依赖 wscat 通过 $WSS_URI 发送数据）" \
            "npm install -g wscat，或在具备 wscat 的环境重跑；本文件不可作为基准结果引用"
        echo "  [跳过] 文件传输 ${size_kb}KB: wscat 不可用（安装: npm install -g wscat）"
        return
    fi

    # 生成测试数据
    local tmpfile
    tmpfile=$(mktemp)
    dd if=/dev/urandom bs=1024 count="$size_kb" of="$tmpfile" 2>/dev/null

    echo "  [WSS] 发送 ${size_kb}KB 文件到 ${WSS_URI}..."
    local start_time end_time elapsed
    start_time=$(date +%s.%N)

    # wscat 发送文件（$WSS_URI 在此前缺失，导致命令拿到空 URI 直接失败）
    if timeout 30 wscat -c "$WSS_URI" --no-color < "$tmpfile" > /tmp/wss_resp.bin 2>/dev/null; then
        end_time=$(date +%s.%N)
        elapsed=$(awk -v a="$start_time" -v b="$end_time" 'BEGIN{printf "%.6f", b-a}')

        local resp_size
        resp_size=$(stat -c%s /tmp/wss_resp.bin 2>/dev/null || echo "0")
        local resp_kb=$((resp_size / 1024))

        {
            echo "目标: ${WSS_URI}"
            echo "发送大小: ${size_kb}KB"
            echo "收到大小: ${resp_kb}KB"
            echo "传输时间: ${elapsed}s"
            echo "说明: wscat 按行读取 stdin，二进制文件传输数字仅供参考，不代表帧级吞吐"
            if awk -v e="$elapsed" 'BEGIN{exit !(e > 0)}'; then
                echo "吞吐量: $(awk -v s="$resp_kb" -v e="$elapsed" 'BEGIN{printf "%.2f", s/e}') KB/s"
            fi
        } >> "$out"

        echo "  文件传输 ${size_kb}KB: $(awk -v s="$resp_kb" -v e="$elapsed" 'BEGIN{printf "%.2f", (e>0)? s/e : 0}') KB/s"
    else
        echo "传输失败（wscat 退出码非 0；确认 ${WSS_URI} 可达，服务端 /echo 路径存在）" >> "$out"
        echo "  文件传输 ${size_kb}KB: 失败"
    fi

    rm -f "$tmpfile" /tmp/wss_resp.bin
}

# ── Main ────────────────────────────────────────────────

echo "=== WSS 基准测试 (分支: $BRANCH) ==="

# 环境探测只做一次（governor 固定尝试可能失败/耗时，缓存到本 shell 供各结果文件头复用）
bench_env_init

# 依赖预检：缺工具时明确跳过，不产出无效/空数据
HAVE_OPENSSL=true
if ! bench_has_tool openssl; then
    HAVE_OPENSSL=false
    echo "[跳过] openssl 不可用：D2(TLS 握手速率)、D3(并发连接) 无法测量"
    echo "       修复: sudo apt install openssl"
fi
if ! bench_has_tool wscat; then
    echo "[跳过] wscat 不可用：D5(文件传输) 跳过；修复: npm install -g wscat"
fi
if [ ! -x "$BUILD_DIR/examples/wss_bench_client" ]; then
    echo "[跳过] $BUILD_DIR/examples/wss_bench_client 不存在：D1(消息吞吐) 跳过"
    echo "       修复: cmake -S . -B $BUILD_DIR -DENABLE_WSS=ON -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR -j2"
fi

# 验证 WSS 端口
if ! timeout 3 bash -c "echo > /dev/tcp/localhost/$WSS_PORT" 2>/dev/null; then
    echo "[错误] WSS 服务器未在端口 $WSS_PORT 运行 —— 本次未产生任何结果"
    echo "       请先启动: $BUILD_DIR/examples/bench_server --port 18080 --wss-port $WSS_PORT --cert <cert> --key <key>"
    echo "       自签证书: openssl req -x509 -newkey rsa:2048 -keyout /tmp/k.pem -out /tmp/c.pem -days 1 -nodes -subj '/CN=localhost'"
    exit 3
fi
echo "WSS 端口 $WSS_PORT 已就绪 (客户端目标: $WSS_URI)"
echo "结果目录: $RESULT_DIR"
echo "环境锚定:"
bench_env_anchor | sed 's/^/  /'

echo ""
echo "--- D1: WSS 消息吞吐量 ---"
bench_throughput 256 "$D1_COUNT" "256B"
bench_throughput 1024 "$D1_COUNT" "1KB"
bench_throughput 16384 "$D1_COUNT" "16KB"

if [ "$HAVE_OPENSSL" = true ]; then
    echo ""
    echo "--- D2: TLS 握手速率 ---"
    bench_handshake "$D2_COUNT"

    echo ""
    echo "--- D3: 并发连接 ---"
    bench_maxconn "$D3_MAXCONN"
else
    echo ""
    echo "--- D2/D3: 跳过 (openssl 不可用) ---"
    for item in "d2_handshake:TLS 握手速率" "d3_maxconn:最大并发 WSS 连接"; do
        skip_id="${item%%:*}"; skip_desc="${item##*:}"
        {
            echo "=== ${skip_id}: ${skip_desc} ==="
            echo "分支: ${BRANCH}"
            bench_env_anchor
            echo "---"
        } > "$RESULT_DIR/${skip_id}.txt"
        bench_write_skip "$RESULT_DIR/${skip_id}.txt" "openssl 不可用" "sudo apt install openssl"
    done
fi

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
grep -H "消息/秒\|吞吐:\|握手/秒\|成功建立\|未能判定\|状态: 跳过" "$RESULT_DIR"/*.txt 2>/dev/null || echo "(无结果)"
