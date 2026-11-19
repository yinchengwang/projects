#!/bin/sh
# ts_bench_io_run.sh —— 跑时序模态的统一 IO 口径基准（W-D 的 D-3.3）
#
# 用法：
#   sh bench/ts_bench_io_run.sh              # 3 个分区 × 9000 行（默认）
#   sh bench/ts_bench_io_run.sh 4            # 4 个分区
#   sh bench/ts_bench_io_run.sh 4 9000       # 分区数 + 每分区行数
#
# ⚠️ 每分区行数必须 < 10000（TS_SEAL_ROW_THRESHOLD）：
#    到阈值就自动封窗，封窗后分区只读、不能再写。
#    传更大的值会被基准自己截到 9999 并打印提示。
#
# ==================== 为什么单独一个脚本 ====================
#
# 判断写在基准里（退出码由基准给），脚本只做三件事：
#   1. 构建（可指定 OBJ_DIR，避免和别的构建抢同一个 .o）
#   2. 跑之前删掉旧的库文件（基准量的是"从零建库"的页数）
#   3. 把基准的退出码原样传出去
#
# 退出码：
#   0 = 四条判据全过
#   1 = 有判据不达标
#   2 = 基准自身出错（开库失败、灌数失败、checkpoint 失败）

ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

DAYS=${1:-3}
ROWS=${2:-9000}

OBJ_DIR=${OBJ_DIR:-build}

# 线程库 / make 的名字在 Windows 和 Linux 上不一样
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        MAKE=mingw32-make
        TS_BENCH_IO_EXE="$OBJ_DIR/ts_bench_io.exe"
        ;;
    *)
        MAKE=make
        TS_BENCH_IO_EXE="$OBJ_DIR/ts_bench_io"
        ;;
esac

mkdir -p "$OBJ_DIR" test/tmp

echo "=== 构建时序 IO 基准（$TS_BENCH_IO_EXE） ==="
$MAKE OBJ_DIR="$OBJ_DIR" "$TS_BENCH_IO_EXE" || exit 2

# 跑之前删掉旧的库文件：基准量的是"从零建库"的页数，带着旧文件跑页数会不对。
rm -f test/tmp/ts_bench_io.db test/tmp/ts_bench_io.db-wal \
      test/tmp/ts_bench_io.db-clog test/tmp/ts_bench_io.db-shm

echo ""
echo "=== 跑基准（分区数 $DAYS，每分区 $ROWS 行） ==="
"$TS_BENCH_IO_EXE" "$DAYS" "$ROWS"
RC=$?

echo ""
if [ "$RC" -eq 0 ]; then
    echo "=== 结论：四条判据全过（退出码 0） ==="
else
    echo "=== 结论：未达标（退出码 $RC） ==="
fi
exit $RC
