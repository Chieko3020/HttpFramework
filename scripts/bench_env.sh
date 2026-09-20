#!/bin/bash
# bench_env.sh — 基准脚本公共函数（结果目录映射 / 环境锚定 / 缺依赖处理）
#
# 用法（在调用脚本里）:
#   PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
#   . "$PROJECT_DIR/scripts/bench_env.sh"
#
# 本文件只有函数定义，没有副作用（source 进来不会执行任何命令）。
# 由 run_http_bench.sh / generate_report.sh 共用，
# 避免"每个脚本各写一份环境记录"导致结果文件口径不一致。
#
# 退出码约定（调用方保持一致）: 0=正常, 3=缺依赖跳过（不是失败）。

# ── 结果目录映射 ──────────────────────────────────────────
#
# 分支名带 '/' 时不能直接拼进路径，所以用显式映射表。未知分支：打印警告后按
# 'results/<分支名，/→->' 使用，保证脚本能跑但不会悄悄写到别的目录。
bench_result_dir() {
    case "${1:-}" in
        main)              printf '%s\n' "results/main" ;;
        "")
            printf '[错误] bench_result_dir: 缺少分支名参数\n' >&2
            return 1 ;;
        *)
            local safe="${1//\//-}"
            printf '[警告] 未知分支 "%s"：映射表未登记，结果目录按默认规则使用 results/%s\n' "$1" "$safe" >&2
            printf 'results/%s\n' "$safe" ;;
    esac
}

# ── 环境探测 ──────────────────────────────────────────────

# CPU 型号（lscpu 摘要）。LC_ALL=C 避免中文 locale 下字段名带全角冒号。
bench_cpu_model() {
    local m
    m=$(LC_ALL=C lscpu 2>/dev/null | awk -F: '/^Model name/{sub(/^[ \t]+/, "", $2); print $2; exit}')
    [ -n "$m" ] || m="未知（lscpu 不可用）"
    printf '%s\n' "$m"
}

# 当前 CPU governor。读不到（虚拟机/容器通常没有 cpufreq 驱动）返回 "不可读"。
bench_governor_read() {
    local f="/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
    if [ -r "$f" ]; then
        cat "$f" 2>/dev/null || printf '不可读\n'
    else
        printf '不可读\n'
    fi
}

# 尝试把 governor 固定为 performance，返回一行描述（调用方不要因为失败而中止）。
# 手段：先写 sysfs（需 root），再退到 cpupower；sudo 用 -n 避免交互式卡住。
bench_governor_pin() {
    local f="/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
    local before after note=""

    before=$(bench_governor_read)
    case "$before" in
        performance)
            printf 'performance（已是 performance，无需调整）\n'
            return 0 ;;
        不可读)
            # 没有 cpufreq 驱动（虚拟机/容器常见），cpupower 也是徒劳，但仍按约定尝试一次
            if command -v cpupower >/dev/null 2>&1; then
                if [ "$(id -u)" -eq 0 ]; then
                    cpupower frequency-set -g performance >/dev/null 2>&1 || true
                else
                    sudo -n cpupower frequency-set -g performance >/dev/null 2>&1 || true
                fi
            fi
            note="sysfs 无 cpufreq 驱动（虚拟机/容器常见）；已尝试 cpupower frequency-set -g performance，无效" ;;
        *)
            if [ -w "$f" ]; then
                echo performance > "$f" 2>/dev/null || true
            elif command -v cpupower >/dev/null 2>&1; then
                if [ "$(id -u)" -eq 0 ]; then
                    cpupower frequency-set -g performance >/dev/null 2>&1 || true
                else
                    sudo -n cpupower frequency-set -g performance >/dev/null 2>&1 || true
                fi
            fi
            note="已尝试写 sysfs/cpupower" ;;
    esac

    after=$(bench_governor_read)
    if [ "$after" = "performance" ]; then
        printf 'performance（已固定；固定前为 %s）\n' "$before"
        return 0
    fi
    printf '[提示] CPU governor 未固定为 performance（当前: %s）：%s\n' "$after" "$note" >&2
    printf '       频率/散热波动会影响不同批次基准的可比性，报告里请保留本行环境信息。\n' >&2
    printf '%s（固定失败：%s）\n' "$after" "$note"
}

# wrk 版本字符串；不可用时明确写"wrk 不可用"，不产出空数据。
bench_wrk_version() {
    if ! command -v wrk >/dev/null 2>&1; then
        printf 'wrk 不可用（未安装）\n'
        return 0
    fi
    local v
    v=$({ wrk --version 2>&1 || wrk -v 2>&1; } 2>/dev/null | head -1 || true)
    [ -n "$v" ] || v="wrk 已安装（版本输出不可用）"
    printf '%s\n' "$v"
}

# 一次性探测并缓存 governor 结果（避免每个结果文件都重试 cpupower）
bench_env_init() {
    if [ -z "${BENCH_GOV_LINE:-}" ]; then
        BENCH_GOV_LINE="$(bench_governor_pin)"
    fi
}

# ── 结果文件头：环境锚定 ──────────────────────────────────
#
# 写出的每一行都是"可复核的锚定信息"：换机器/换编译器/换 CPU 调频状态后，
# 旧结果文件不再可被当成同一条件下的数据引用。
bench_env_anchor() {
    local root="${PROJECT_DIR:-$(pwd)}"
    local cpus mem head
    cpus=$(nproc 2>/dev/null || echo '?')
    mem=$(awk '/^MemTotal/{printf "%.0f", $2/1024}' /proc/meminfo 2>/dev/null || true)
    [ -n "$mem" ] || mem="?"
    head=$(git -C "$root" rev-parse HEAD 2>/dev/null || true)
    [ -n "$head" ] || head="未知（非 git 仓库或无提交）"

    bench_env_init

    echo "环境: ${cpus} vCPU / ${mem} MB / $(uname -sr)"
    echo "环境-CPU: $(bench_cpu_model) × ${cpus}"
    echo "环境-gcc: $(g++ --version 2>/dev/null | head -1 || echo 'g++ 不可用')"
    echo "环境-git-HEAD: ${head}"
    echo "环境-wrk: $(bench_wrk_version)"
    echo "环境-governor: ${BENCH_GOV_LINE}"
    echo "环境-记录时间: $(date -Iseconds)"
}

# ── 缺依赖：写"跳过+原因+修复"，而不是产出 0/空数据 ────────
bench_write_skip() {
    local out="$1" reason="$2" fix="${3:-}"
    {
        echo "状态: 跳过（缺依赖）"
        echo "原因: ${reason}"
        [ -n "$fix" ] && echo "修复: ${fix}"
        echo "说明: 本次未产生任何测量数据，本文件不得作为基准结果引用"
    } >> "$out" 2>/dev/null || true
}

# 打印缺依赖提示并让调用方决定退出；返回 0=工具可用, 1=不可用
bench_has_tool() {
    command -v "$1" >/dev/null 2>&1
}
