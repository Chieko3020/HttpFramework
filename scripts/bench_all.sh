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

# 公共函数：分支 → 结果目录映射 + 结果文件环境锚定
# （三个脚本共用一份实现，避免 results/feature/WebSocket 这类拼路径错误）
. "$PROJECT_DIR/scripts/bench_env.sh"

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

    if ! git checkout "$BRANCH" 2>/tmp/bench_checkout.err; then
        echo "[错误] 无法切换到分支 $BRANCH："
        cat /tmp/bench_checkout.err
        echo "       本地/远程都没有该分支，或工作区改动会被覆盖（先 git status 确认）"
        exit 1
    fi

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

    # 结果目录：走映射表（main → results/main，feature/WebSocket → results/wss）；
    # 直接拼 "results/$BRANCH" 会得到 results/feature/WebSocket，与实际目录不符
    if ! RESULT_DIR=$(bench_result_dir "$BRANCH"); then
        echo "[错误] 分支 $BRANCH 未登记在 scripts/bench_env.sh 的 bench_result_dir 映射表里"
        echo "       请先登记映射，避免结果写到非预期目录"
        exit 1
    fi
    LABEL="$BRANCH"
    echo "[结果] $BRANCH → $RESULT_DIR"

    # ── HTTP 基准 (两个分支都做) ──────────────────────────

    echo ""
    echo "[HTTP] 开始 HTTP 基准测试..."
    chmod +x scripts/run_http_bench.sh
    HTTP_RC=0
    bash scripts/run_http_bench.sh "$LABEL" "$RESULT_DIR" || HTTP_RC=$?
    if [ "$HTTP_RC" -eq 3 ]; then
        echo "[HTTP] ⏭️  跳过（缺依赖，例如未安装 wrk / bench_server 未编译）；继续后续分支"
    elif [ "$HTTP_RC" -ne 0 ]; then
        echo "[HTTP] ❌ 基准测试失败（退出码 $HTTP_RC）"
        exit "$HTTP_RC"
    else
        echo "[HTTP] ✅ HTTP 基准测试完成"
    fi

    # ── WSS 基准 (仅 feature/WebSocket) ────────────────────

    if [ "$BRANCH" = "feature/WebSocket" ] && [ "$SKIP_WSS" = false ]; then
        echo ""
        echo "[WSS] 开始 WSS 基准测试..."

        # 启动带 WSS 的 bench_server
        HTTP_PORT=18080
        WSS_PORT=18990
        mkdir -p /tmp/hf-test
        echo "[WSS] 启动 bench_server (HTTP=$HTTP_PORT, WSS=$WSS_PORT)"
        "$BUILD_DIR/examples/bench_server" \
            --port "$HTTP_PORT" \
            --wss-port "$WSS_PORT" \
            --cert /tmp/bench_cert.pem \
            --key /tmp/bench_key.pem \
            --threads 4 > /tmp/hf-test/bench_server_wss.log 2>&1 &
        SERVER_PID=$!

        # 轮询等待就绪（最多 10s），不用固定 sleep 猜启动时间
        WSS_READY=false
        for _ in $(seq 1 50); do
            if curl -s -o /dev/null -w "%{http_code}" "http://localhost:$HTTP_PORT/bench/plaintext" 2>/dev/null | grep -q 200; then
                WSS_READY=true
                break
            fi
            if ! kill -0 "$SERVER_PID" 2>/dev/null; then
                echo "[WSS] ❌ 服务器进程已退出，日志尾部:"
                tail -5 /tmp/hf-test/bench_server_wss.log 2>/dev/null
                break
            fi
            sleep 0.2
        done

        if [ "$WSS_READY" = true ]; then
            echo "[WSS] ✅ 服务器就绪"

            # D4: HTTP+WSS 共存测试
            echo "[WSS] D4: HTTP 压测 + WSS 消息同时运行"
            mkdir -p "$RESULT_DIR"
            {
                echo "=== D4: HTTP+WSS 共存 ==="
                echo "分支: $BRANCH"
                echo "时间: $(date -Iseconds)"
                bench_env_anchor
                echo "---"
                echo "说明: 后台 wrk 压 HTTP ($HTTP_PORT) 的同时跑 run_wss_bench.sh (WSS $WSS_PORT)"
            } > "$RESULT_DIR/d4_coexist.txt"

            # 后台启动 HTTP 压测
            wrk -t2 -c100 -d20s "http://localhost:$HTTP_PORT/bench/json" > /tmp/wrk_coexist.txt 2>&1 &
            WRK_PID=$!

            # 同时运行 WSS 基准（D1/D2/D3 用 openssl + C++ 客户端，D5 需要 wscat）
            if ! command -v openssl >/dev/null 2>&1 || [ ! -x "$BUILD_DIR/examples/wss_bench_client" ]; then
                echo "[WSS] 跳过（缺 openssl 或 wss_bench_client 未编译）"
                {
                    echo "状态: 跳过（缺依赖）"
                    echo "原因: openssl 不可用或 $BUILD_DIR/examples/wss_bench_client 未编译"
                    echo "修复: cmake -S . -B $BUILD_DIR -DENABLE_WSS=ON -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR -j2"
                    echo "说明: 本次未产生任何测量数据，本文件不得作为基准结果引用"
                } >> "$RESULT_DIR/d4_coexist.txt"
            else
                WSS_RC=0
                BUILD_DIR="$BUILD_DIR" WSS_PORT="$WSS_PORT" \
                    bash "$PROJECT_DIR/scripts/run_wss_bench.sh" "$LABEL" "$RESULT_DIR" 2>&1 | tee -a "$RESULT_DIR/d4_coexist.txt" || WSS_RC=$?
                [ "$WSS_RC" -eq 0 ] || echo "[WSS] ⚠️  run_wss_bench.sh 退出码 $WSS_RC（3 = 缺依赖跳过）"
            fi

            wait $WRK_PID 2>/dev/null || true
            cat /tmp/wrk_coexist.txt >> "$RESULT_DIR/d4_coexist.txt"
            echo "[WSS] ✅ WSS 基准测试完成"
        else
            echo "[WSS] ❌ 服务器 10s 内未就绪，D4 跳过（日志: /tmp/hf-test/bench_server_wss.log）"
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
echo ""
echo "退出码约定: 0=正常，3=缺依赖跳过（例如未安装 wrk）"
