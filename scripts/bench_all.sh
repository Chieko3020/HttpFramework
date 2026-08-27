#!/bin/bash
# bench_all.sh — 双分支全量基准测试
# 对 main (纯HTTP) 和 feature/WebSocket (HTTP+WSS) 分别运行所有基准测试
#
# 用法:
#   ./scripts/bench_all.sh          # 测试两个分支
#   ./scripts/bench_all.sh --skip-wss  # 仅 HTTP (适用于 main 分支没有 WSS 的情况)
#
# 输出:
#   results/main/   — main 分支的 HTTP 基准结果
#   results/wss/    — feature/WebSocket 分支的 HTTP+WSS 基准结果

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

BRANCHES=("main" "feature/WebSocket")
SKIP_WSS=false

for arg in "$@"; do
    case $arg in
        --skip-wss) SKIP_WSS=true ;;
        *) echo "未知选项: $arg"; exit 1 ;;
    esac
done

# 保存当前分支以便恢复
ORIG_BRANCH=$(git branch --show-current)

echo "╔══════════════════════════════════════════╗"
echo "║   HttpFramework 双分支性能基准测试         ║"
echo "╚══════════════════════════════════════════╝"
echo ""
echo "开始时间: $(date)"
echo ""

# ── 生成 TLS 证书 (WSS 测试用) ──────────────────────────

generate_certs() {
    if [ ! -f /tmp/bench_cert.pem ] || [ ! -f /tmp/bench_key.pem ]; then
        echo "[准备] 生成自签名 TLS 证书..."
        openssl req -x509 -newkey rsa:2048 -keyout /tmp/bench_key.pem \
            -out /tmp/bench_cert.pem -days 1 -nodes \
            -subj '/CN=localhost' 2>/dev/null
        echo "[准备] 证书已生成: /tmp/bench_cert.pem /tmp/bench_key.pem"
    fi
}

generate_certs

for BRANCH in "${BRANCHES[@]}"; do
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  分支: $BRANCH"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    git checkout "$BRANCH"

    # 确定 CMake 选项
    CMAKE_OPTS="-DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=ON"
    if [ "$BRANCH" = "feature/WebSocket" ]; then
        CMAKE_OPTS="$CMAKE_OPTS -DENABLE_WSS=ON"
    fi

    BUILD_DIR="build"
    echo "[构建] cmake -S . -B $BUILD_DIR $CMAKE_OPTS"
    if ! cmake -S . -B "$BUILD_DIR" $CMAKE_OPTS > "$BUILD_DIR/build.log" 2>&1; then
        echo "[构建] ❌ CMake 配置失败!"
        tail -20 "$BUILD_DIR/build.log"
        exit 1
    fi
    echo "[构建] cmake --build $BUILD_DIR -j 2"
    if ! cmake --build "$BUILD_DIR" -j 2 >> "$BUILD_DIR/build.log" 2>&1; then
        echo "[构建] ❌ 编译失败!"
        tail -20 "$BUILD_DIR/build.log"
        exit 1
    fi

    if ! grep -i warning "$BUILD_DIR/build.log" | grep -v "WARN\|CMAKE" > /dev/null 2>&1; then
        echo "[构建] ✅ 编译完成, 0 警告"
    else
        echo "[构建] ⚠️  编译有警告:"
        grep -i warning "$BUILD_DIR/build.log" | grep -v "WARN\|CMAKE" | head -5
    fi

    RESULT_DIR="results/$BRANCH"
    LABEL="$BRANCH"

    # ── HTTP 基准 (两个分支都做) ──────────────────────────

    echo ""
    echo "[HTTP] 开始 HTTP 基准测试..."
    chmod +x scripts/run_http_bench.sh
    bash scripts/run_http_bench.sh "$LABEL" "$RESULT_DIR"
    echo "[HTTP] ✅ HTTP 基准测试完成"

    # ── WSS 基准 (仅 feature/WebSocket) ────────────────────

    if [ "$BRANCH" = "feature/WebSocket" ] && [ "$SKIP_WSS" = false ]; then
        echo ""
        echo "[WSS] 开始 WSS 基准测试..."

        # 启动带 WSS 的 bench_server
        HTTP_PORT=18080
        WSS_PORT=18990
        echo "[WSS] 启动 bench_server (HTTP=$HTTP_PORT, WSS=$WSS_PORT)"
        "$BUILD_DIR/examples/bench_server" \
            --port "$HTTP_PORT" \
            --wss-port "$WSS_PORT" \
            --cert /tmp/bench_cert.pem \
            --key /tmp/bench_key.pem \
            --threads 4 > /dev/null 2>&1 &
        SERVER_PID=$!
        sleep 3

        # 验证 WSS 端口可用
        if curl -s -o /dev/null -w "%{http_code}" "http://localhost:$HTTP_PORT/bench/plaintext" | grep -q 200; then
            echo "[WSS] ✅ 服务器就绪"

            # D4: HTTP+WSS 共存测试
            echo "[WSS] D4: HTTP 压测 + WSS 消息同时运行"
            mkdir -p "$RESULT_DIR"
            {
                echo "=== D4: HTTP+WSS 共存 ==="
                echo "时间: $(date -Iseconds)"
                echo "---"
                echo "HTTP 压测 (wrk) 与 WSS Echo (Python) 同时运行"
            } > "$RESULT_DIR/d4_coexist.txt"

            # 后台启动 HTTP 压测
            wrk -t2 -c100 -d20s "http://localhost:$HTTP_PORT/bench/json" > /tmp/wrk_coexist.txt 2>&1 &
            WRK_PID=$!

            # 同时运行 WSS 吞吐量
            if python3 -c "import websockets" 2>/dev/null; then
                bash "$PROJECT_DIR/scripts/run_wss_bench.sh" "$LABEL" "$RESULT_DIR" 2>&1 | tee -a "$RESULT_DIR/d4_coexist.txt" || true
            else
                echo "[WSS] 跳过 (npx wscat 不可用)"
                echo "需要安装: npm install -g wscat 或 npx wscat" >> "$RESULT_DIR/d4_coexist.txt"
            fi

            wait $WRK_PID 2>/dev/null || true
            cat /tmp/wrk_coexist.txt >> "$RESULT_DIR/d4_coexist.txt"
            echo "[WSS] ✅ WSS 基准测试完成"
        else
            echo "[WSS] ❌ 服务器启动失败"
        fi

        kill $SERVER_PID 2>/dev/null || true
        wait $SERVER_PID 2>/dev/null || true

    elif [ "$BRANCH" = "main" ] && [ "$SKIP_WSS" = false ]; then
        echo ""
        echo "[WSS] 跳过 (main 分支不支持 WSS)"
    fi

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  $BRANCH 完成"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
done

# ── 恢复原始分支 ────────────────────────────────────────

git checkout "$ORIG_BRANCH"

echo ""
echo "╔══════════════════════════════════════════╗"
echo "║   全量基准测试完成                          ║"
echo "╚══════════════════════════════════════════╝"
echo ""
echo "结果目录:"
echo "  results/main/  — main 分支 (纯 HTTP)"
echo "  results/wss/   — feature/WebSocket 分支 (HTTP + WSS)"
echo ""
echo "生成报告:"
echo "  bash scripts/generate_report.sh"
