#!/bin/sh
# latch_bench_run.sh —— A3 对照：细粒度 latch vs 大锁基线
#
# 用法：
#   sh bench/latch_bench_run.sh [每表行数] [每线程轮数] [线程数]
#   默认 50000 / 30 / 4
#
# ==================== 这个脚本在做什么 ====================
#
#   1. 构建两个基准二进制 —— **同一份源码**，只差一个编译期宏
#      （-DDB_BIGLOCK_BASELINE，把退役的全局大锁原样放回去）
#   2. 各自在一个**全新的库文件**上跑一遍（跑之前先删旧文件）
#   3. 抓两边的 ops_per_sec，算比值，给出 A3 的判据（≥ 2 倍）
#
# ==================== 为什么要"两次独立运行" ====================
#
# 两个二进制分别跑再比，中间隔着：两次进程启动、两次页缓存冷启动、
# 两次 WAL 恢复。任何一处不确定（比如缓冲池里残留的脏页）都可能让两次
# 跑出不同的结果，而差异会被误判成"latch 带来的差异" —— 假阳性比漏检更坏。
#
# ==================== 口径说明 ====================
#
# 计划 A3 写的"基线 v1.1 已存档"其实指的是**执行器**对照基线（PLAN-v1.1 §3 的
# 火山执行器冻结副本），**不是**大锁基线 —— 仓库里没有存过大锁版本的二进制。
# 所以这里的大锁基线是**当场重建**的：同一份源码 + 编译期开关，
# 等价于步骤 4d 退役大锁之前的行为。这一点在 PLAN-v1.2.md 里有勘误说明。

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

ROWS=${1:-50000}
ROUNDS=${2:-30}
THREADS=${3:-4}

# 线程库 / make 的名字在 Windows 与 Linux 上不一样
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        MAKE=mingw32-make
        FINE=build/latch_bench.exe
        BIG=build/latch_bench_biglock.exe
        ;;
    *)
        MAKE=make
        FINE=build/latch_bench
        BIG=build/latch_bench_biglock
        ;;
esac

mkdir -p build test/tmp

echo "=== 构建两个基准二进制（同一份源码，只差 -DDB_BIGLOCK_BASELINE） ==="
$MAKE $FINE $BIG

OUT_FINE=build/latch_bench_fine.out
OUT_BIG=build/latch_bench_biglock.out

# 跑一个二进制，输出写进 $3，最后一行是 ops_per_sec
run_one() {
    bin="$1"
    label="$2"
    out="$3"

    rm -f test/tmp/latch_bench.db test/tmp/latch_bench.db-wal \
          test/tmp/latch_bench.db-clog test/tmp/latch_bench.db-shm

    {
        echo "--- $label ---"
        "$bin" "$ROWS" "$ROUNDS" "$THREADS"
    } > "$out" 2>&1

    sed -n 's/.*ops_per_sec=\([0-9.]*\).*/\1/p' "$out" | tail -1
}

echo ""
echo "=== 跑细粒度版本 ==="
FINE_OPS=$(run_one "$FINE" "细粒度 latch（当前源码）" "$OUT_FINE")

echo "=== 跑大锁基线版本 ==="
BIG_OPS=$(run_one "$BIG" "大锁基线（-DDB_BIGLOCK_BASELINE）" "$OUT_BIG")

echo ""
echo "==================== 两次运行的完整输出 ===================="
cat "$OUT_FINE"
cat "$OUT_BIG"

echo ""
echo "==================== A3 判据 ===================="
echo "  细粒度 : ${FINE_OPS} 条语句/秒"
echo "  大锁基线: ${BIG_OPS} 条语句/秒"

RATIO=$(awk -v a="$FINE_OPS" -v b="$BIG_OPS" 'BEGIN{ if (b+0 > 0) printf "%.2f", a/b; else print "0.00" }')
echo "  比值   : ${RATIO}x"

if awk -v r="$RATIO" 'BEGIN{ exit !(r+0 >= 2.0) }'; then
    echo "  ✓ 达标（A3 要求 ≥ 2x）"
    exit 0
else
    echo "  ✗ 未达标（A3 要求 ≥ 2x）"
    exit 1
fi
