#!/bin/bash
# generate_report.sh — 解析基准结果文件，生成 HTTP 基准报告 (纯 Shell)
# 用法: ./scripts/generate_report.sh [results_dir] [output_file]
# 默认: 读取 results/ 目录，输出到 results/REPORT.md
#
# 原则：报告里的每一个结论都必须能从结果文件里算出来。
#       算不出来（结果文件缺失/旧格式没有环境锚定行）就写"数据缺失"，
#       不允许出现硬编码的硬件描述或效果百分比。
# 依赖: awk / grep / sed（不依赖 jq、不依赖 bc）

set -eo pipefail

RESULTS_DIR="${1:-results}"
OUT_FILE="${2:-$RESULTS_DIR/REPORT.md}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# 公共函数：分支 → 结果目录映射（与 run_http_bench 共用一份）
. "$PROJECT_DIR/scripts/bench_env.sh"
MAIN_DIR="$RESULTS_DIR/$(basename "$(bench_result_dir main)")"

# ── 通用提取函数 ──────────────────────────────────────────

# 取匹配行的值（字面量前缀匹配，避免正则转义问题）；无匹配输出空
line_value() {
    [ -f "$1" ] || return 0
    awk -v lab="$2" 'index($0, lab) == 1 { sub(/^[^:]*:[ \t]*/, ""); print; exit }' "$1" 2>/dev/null || true
}

# 带"数据缺失"兜底的取值
value_or_missing() {
    local v
    v=$(line_value "$1" "$2")
    [ -n "$v" ] && printf '%s\n' "$v" || printf '数据缺失\n'
}

# 空值兜底（提取函数取不到值时统一显示"数据缺失"）
present() {
    if [ -n "${1:-}" ]; then printf '%s\n' "$1"; else printf '数据缺失\n'; fi
}

rps() { grep -oP 'Requests/sec:\s*\K[\d.]+' "$1" 2>/dev/null | head -1 || true; }

# 提取标记行之后第一个 Requests/sec 值
extract_rps() {
    awk "BEGIN{found=0} /$1/{found=1; next} found && /Requests\/sec:/{print \$2; found=0; exit}" "$2" 2>/dev/null || true
}

latency_at() {
    grep -oP "$1%\s+\K[\d.]+\s*\w+" "$2" 2>/dev/null | head -1 || true
}

latency_avg() {
    grep -oP 'Latency\s+\K[\d.]+\s*\w+' "$1" 2>/dev/null | head -1 || true
}

rss_peak() {
    local kb
    kb=$(grep VmRSS "$1" 2>/dev/null | tail -1 | awk '{print $2}' || true)
    [ -n "$kb" ] && awk "BEGIN{printf \"%.1f MB\", $kb/1024}" || printf '数据缺失\n'
}

# 取 "并发: N" 之后的 Socket errors 行（用于判断是否出现连接/读超时错误）
socket_errors_after() {
    awk -v m="$2" 'index($0, m){f=1; next} f && /Socket errors/{print $0; exit}' "$1" 2>/dev/null \
        | sed 's/^[[:space:]]*Socket errors:[[:space:]]*//' || true
}

# 百分比差异：新值相对基准值的变化；缺数据输出"数据缺失"
pct_diff() {
    awk -v b="${1:-}" -v n="${2:-}" 'BEGIN{
        if (b == "" || n == "" || b !~ /^[0-9.]+$/ || n !~ /^[0-9.]+$/ || b + 0 == 0) { print "数据缺失"; exit }
        printf "%+.1f%%", (n - b) / b * 100
    }'
}

# 单个结果文件的环境锚定值（无 环境-git-HEAD 行 → 空 = 未锚定）
file_head() {
    grep -oP '^环境-git-HEAD:[ \t]*\K.*' "$1" 2>/dev/null | head -1 || true
}

# 带锚定的百分比差异：两个结果文件都必须存在、都带 环境-git-HEAD 行，且
# **HEAD 相同**，才输出百分比；否则输出"数据缺失（结果文件未锚定）"。
# 动机：仓库里 results/ 下有一批 2026-05 入库、无环境行的旧 txt，用它们"实算"
# 出的收益数字无法与当前代码对应（曾经算出"内存池 +55.4%"，而交替复测结论是
# 内存池无收益），这类数字不能出现在结论里。
anchored_pct() {
    local f1="$1" f2="$2" v1="$3" v2="$4" h1 h2
    if [ ! -f "$f1" ] || [ ! -f "$f2" ]; then echo "数据缺失（结果文件不存在）"; return; fi
    h1=$(file_head "$f1"); h2=$(file_head "$f2")
    if [ -z "$h1" ] || [ -z "$h2" ]; then
        echo "数据缺失（结果文件未锚定：缺 环境-git-HEAD 行）"; return
    fi
    if [ "$h1" != "$h2" ]; then
        echo "数据缺失（两侧 HEAD 不同：${h1} vs ${h2}）"; return
    fi
    pct_diff "$v1" "$v2"
}

# 结果文件里的环境锚定字段（取第一个非空值；旧结果文件没有这些行 → 数据缺失）
env_field() {
    local v
    v=$(grep -rhoP "^$1:[ \t]*\K.*" --include='*.txt' "$RESULTS_DIR" 2>/dev/null | head -1 || true)
    [ -n "$v" ] && printf '%s\n' "$v" || printf '数据缺失\n'
}

# ── 生成报告 ──────────────────────────────────────────────

{
echo "# HttpFramework 性能基准测试报告"
echo ""
echo "> 生成时间: $(date '+%Y-%m-%d %H:%M')"
echo "> 分支 → 结果目录: main → ${MAIN_DIR}"
echo ""
echo "## 环境锚定（逐项取自结果文件头，缺失即写\"数据缺失\"）"
echo ""
echo "| 项目 | 值 |"
echo "|------|-----|"
echo "| CPU | $(env_field 环境-CPU) |"
echo "| 机器/内存 | $(env_field 环境) |"
echo "| 编译器 | $(env_field 环境-gcc) |"
echo "| git HEAD | $(env_field 环境-git-HEAD) |"
echo "| wrk | $(env_field 环境-wrk) |"
echo "| CPU governor | $(env_field 环境-governor) |"
echo ""
echo "> 说明：早于环境锚定改造的结果文件没有 环境-* 行，本表会显示\"数据缺失\"，"
echo "> 这类文件不能与新结果直接比较。所有 \"+x.x%\" 形式的结论都只由"
echo "> **两侧都带 环境-git-HEAD 且 HEAD 一致**的结果文件算出；否则该位置写"
echo "> \"数据缺失（结果文件未锚定）\"，不产出任何收益数字。"
echo ""

# ── A1 ──
echo "## A1: 纯文本吞吐量"
echo ""
echo "| 场景 | req/s |"
echo "|------|-------|"
for item in "100 并发:a1_plaintext" "1000 并发:a1_plaintext_hc"; do
    label="${item%%:*}"; f="${item##*:}"
    m=$(rps "$MAIN_DIR/$f.txt")
    echo "| $label | ${m:-数据缺失} |"
done
echo ""

# ── A2 ──
echo "## A2: JSON 响应吞吐量"
echo ""
m=$(rps "$MAIN_DIR/a2_json.txt")
echo "| 场景 | req/s |"
echo "|------|-------|"
echo "| JSON 响应 | ${m:-数据缺失} |"
echo ""

# ── A3 ──
echo "## A3: 延迟分布 (100 conn, JSON)"
echo ""
echo "| 分位 | 延迟 |"
echo "|------|------|"
echo "| avg | $(present "$(latency_avg "$MAIN_DIR/a3_latency.txt")") |"
echo "| p50 | $(present "$(latency_at 50 "$MAIN_DIR/a3_latency.txt")") |"
echo "| p99 | $(present "$(latency_at 99 "$MAIN_DIR/a3_latency.txt")") |"
echo ""

# ── A4 ──
echo "## A4: 最大并发连接"
echo ""
echo "| 并发数 | req/s |"
echo "|--------|-------|"
for conn in 100 500 1000 2000 5000; do
    mc=$(extract_rps "并发: $conn" "$MAIN_DIR/a4_maxconn.txt")
    echo "| $conn | ${mc:-数据缺失} |"
done
echo ""
echo "压测期 Socket errors（非 0 说明出现连接失败/读错误，不能只写\"稳定\"）："
echo ""
echo "- 5000 并发: $(present "$(socket_errors_after "$MAIN_DIR/a4_maxconn.txt" "并发: 5000")")"
echo ""

# ── A5 ──
echo "## A5: 内存占用"
echo ""
mr=$(rss_peak "$MAIN_DIR/a5_memory.txt")
echo "| 指标 | 值 |"
echo "|------|-----|"
echo "| 峰值 RSS | ${mr:-数据缺失} |"
echo ""

# ── A6 ──
echo "## A6: 每请求 CPU 成本"
echo ""
echo "- 每请求 CPU 时间: $(value_or_missing "$MAIN_DIR/a6_cpu_cost.txt" "每请求 CPU 时间")"
echo ""

# ── B1 ──
echo "## B1: 线程扩展性"
echo ""
echo "| 线程数 | req/s |"
echo "|--------|-------|"
for t in 1 2 4 8 16; do
    mt=$(extract_rps "线程: $t" "$MAIN_DIR/b1_threads.txt")
    echo "| $t | ${mt:-数据缺失} |"
done
echo ""

# ── B2 ──
echo "## B2: 内存池收益"
echo ""
mo=$(rps "$MAIN_DIR/b2_mempool_off.txt")
mn=$(rps "$MAIN_DIR/b2_mempool_on.txt")
echo "| 配置 | req/s |"
echo "|------|-------|"
echo "| OFF | ${mo:-数据缺失} |"
echo "| ON  | ${mn:-数据缺失} |"
echo ""
echo "- 内存池收益: $(anchored_pct "$MAIN_DIR/b2_mempool_off.txt" "$MAIN_DIR/b2_mempool_on.txt" "$mo" "$mn")"
echo ""
echo "> 口径：本段只在两侧结果文件都带 环境-git-HEAD 且 HEAD 一致时才给百分比。"
echo "> 早期入库的 b2 结果（无 环境-* 行）不足以下收益结论 —— 需要时请重跑"
echo "> scripts/run_http_bench.sh 的 B2 段落后再看这一节。"
echo ""

# ── B3 ──
echo "## B3: 路由扩展性"
echo ""
echo "| 路由数 | req/s |"
echo "|--------|-------|"
for n in 10 100 500 1000; do
    mr=$(extract_rps "路由数: $n" "$MAIN_DIR/b3_routes.txt")
    echo "| $n | ${mr:-数据缺失} |"
done
echo ""

# ── C1 ──
echo "## C1: 中间件开销"
echo ""
echo "| 层数 | req/s |"
echo "|------|-------|"
for m in 0 1 5 10; do
    mm=$(extract_rps "中间件层数: $m" "$MAIN_DIR/c1_middleware.txt")
    echo "| $m | ${mm:-数据缺失} |"
done
echo ""

# ── C2 ──
echo "## C2: 会话开销"
echo ""
mo=$(rps "$MAIN_DIR/c2_session_off.txt")
mn=$(rps "$MAIN_DIR/c2_session_on.txt")
echo "| 配置 | req/s |"
echo "|------|-------|"
echo "| OFF | ${mo:-数据缺失} |"
echo "| ON  | ${mn:-数据缺失} |"
echo ""

# ── 结论 ──
echo "## 总结（全部由结果文件计算，缺失写\"数据缺失\"）"
echo ""
echo "| 指标 | 数值 |"
echo "|------|------|"
mhc=$(rps "$MAIN_DIR/a1_plaintext_hc.txt")
echo "| HTTP 纯文本吞吐 (1000 conn) | ${mhc:-数据缺失} req/s |"
echo "| HTTP p50 延迟 (100 conn) | $(present "$(latency_at 50 "$MAIN_DIR/a3_latency.txt")") |"
mo=$(rps "$MAIN_DIR/b2_mempool_off.txt"); mn=$(rps "$MAIN_DIR/b2_mempool_on.txt")
echo "| 内存池加速 | $(anchored_pct "$MAIN_DIR/b2_mempool_off.txt" "$MAIN_DIR/b2_mempool_on.txt" "$mo" "$mn")（off ${mo:-数据缺失} → on ${mn:-数据缺失} req/s） |"
echo "| 5000 并发 | $(present "$(extract_rps "并发: 5000" "$MAIN_DIR/a4_maxconn.txt")") req/s；Socket errors: $(present "$(socket_errors_after "$MAIN_DIR/a4_maxconn.txt" "并发: 5000")") |"
echo ""
echo "> 本报告不输出\"稳定无崩溃\"\"影响 <2%\"这类无法从结果文件核验的结论；"
echo "> 需要这类判断时，请以 A4 的 Socket errors 行和重复测量的一致性为依据。"
echo ""

}> "$OUT_FILE"

echo "报告已生成: $OUT_FILE"
cat "$OUT_FILE"
