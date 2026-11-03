#!/bin/sh
# 反复跑 A3 对照，取中位数 —— 单次运行的波动有 ±30%，单次数值不可信
set -e
cd "$(dirname "$0")/.."
ROWS="${1:-2000}"
ROUNDS="${2:-200}"
THREADS="${3:-4}"
N="${4:-3}"

FINE=build/latch_bench
BIG=build/latch_bench_biglock
[ -x "$FINE" ] || FINE="$FINE.exe"
[ -x "$BIG" ] || BIG="$BIG.exe"

run() {
    rm -f test/tmp/latch_bench.db
    "$1" "$ROWS" "$ROUNDS" "$THREADS" 2>/dev/null | grep '^RESULT' | sed 's/.*ops_per_sec=\([0-9.]*\).*/\1/'
}

i=1
while [ "$i" -le "$N" ]; do
    f=$(run "$FINE")
    b=$(run "$BIG")
    echo "第 $i 轮: 细粒度 $f / 大锁 $b"
    i=$((i + 1))
done
