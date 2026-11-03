#!/bin/sh
# ft_bench_run.sh —— v1.3 步骤 5：全文检索的基准（A4 合并代价 + A5 IO 可证）
#
# 用法：
#   sh bench/ft_bench_run.sh [a4|a5|all] [文档数] [批大小]
#   默认 all / 100000 / 1000
#
# ==================== 退出码 ====================
#
#   0 = 全部判据达标
#   1 = 有判据不达标（A4 或 A5）
#   2 = 基准自身出错（开库失败、语料生成失败……）
#
# ==================== 为什么单独一个脚本 ====================
#
# 判据写在基准里（退出码由基准给），脚本只做三件事：
#   1. 构建（可指定 OBJ_DIR，避免和别的构建抢同一个 .o）
#   2. 跑之前删掉旧的库文件（基准要"从零开始"，否则页数不对）
#   3. 把基准的退出码原样传出去
#
# OBJ_DIR 可以用环境变量覆盖（默认 build）：
#   OBJ_DIR=build_ft sh bench/ft_bench_run.sh

ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

MODE=${1:-all}
NDOCS=${2:-100000}
BATCH=${3:-1000}

OBJ_DIR=${OBJ_DIR:-build}

# 线程库 / make 的名字在 Windows 与 Linux 上不一样
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        MAKE=mingw32-make
        FT_BENCH_EXE="$OBJ_DIR/ft_bench.exe"
        ;;
    *)
        MAKE=make
        FT_BENCH_EXE="$OBJ_DIR/ft_bench"
        ;;
esac

mkdir -p "$OBJ_DIR" test/tmp

echo "=== 构建全文检索基准（$FT_BENCH_EXE） ==="
$MAKE OBJ_DIR="$OBJ_DIR" "$FT_BENCH_EXE" || exit 2

# 跑之前删掉旧的库文件：基准量的是"从零建库"的页数，
# 带着旧文件跑，页数与页读都会不对。
rm -f test/tmp/ft_bench_a4.db test/tmp/ft_bench_a4.db-wal \
      test/tmp/ft_bench_a4.db-clog test/tmp/ft_bench_a4.db-shm \
      test/tmp/ft_bench_a5.db test/tmp/ft_bench_a5.db-wal \
      test/tmp/ft_bench_a5.db-clog test/tmp/ft_bench_a5.db-shm

echo ""
echo "=== 跑基准（模式 $MODE，文档数 $NDOCS，批大小 $BATCH） ==="
"$FT_BENCH_EXE" "$MODE" "$NDOCS" "$BATCH"
RC=$?

echo ""
if [ "$RC" -eq 0 ]; then
    echo "=== 结论：全部判据达标（退出码 0） ==="
else
    echo "=== 结论：未达标（退出码 $RC） ==="
fi
exit $RC
