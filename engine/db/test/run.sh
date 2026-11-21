#!/bin/sh
# 编译并运行 test/unit 与 test/integration 下的 test_*.c
#
# 用法：
#   sh test/run.sh              # 并行，默认 4 路
#   JOBS=1 sh test/run.sh       # 串行（排障时用）
#   JOBS=8 sh test/run.sh       # 更宽
#   WITH_SAN=1 sh test/run.sh   # 开 ASan + UBSan（MinGW 常缺 libasan）
#
# ==================== 为什么可以并行 ====================
#
# 每个测试是**独立的可执行文件**，且互相不碰对方的路径：
#   - 每个测试用自己的 test/tmp/<某名>.db（实测 93 个测试里
#     没有任何两个共用同一个 .db 路径，也没有共用非 .db 的临时文件）
#   - 编译产物是 build/<测试名>，一名一个
#   - 没有网络端口、没有 fork 子进程、没有固定等待
#
# 所以并行只共享 CPU 与文件系统，不共享状态。
#
# 实测：串行全量约 3 分钟，其中大半是 93 次「编译 + 链接」的固定开销
# （单次约 0.55 秒 = 约 51 秒），而最慢的单个测试 test_ts_seal 才 0.83 秒。
#
# ==================== 并行方式：分块，不是流水线 ====================
#
# 每 JOBS 个测试起一批，全部 `&` 后 `wait` 收齐，再起下一批。
#
# 为什么不上「谁先完成谁补位」的流水线：
#   那需要 `wait -n`（BusyBox / 老 bash 没有）或轮询 pid（每个测试
#   一次 kill -0，93 个测试 × N 轮轮询，代码复杂且是 sh 兼容性雷区）。
#   分块的代价只是「一批里最慢的那个拖住其余槽位」—— 实测最大
#   测试 0.83 秒、平均 0.3 秒，浪费远小于实现风险。
#
# ==================== 输出为什么不再逐行打 ====================
#
# 并行时多行输出会互相穿插，"哪个测试失败了"反而看不清。
# 所以每个测试的完整输出落到 build/test-logs/<名字>.log，
# 终端每条只打一行结论；失败时再把这些日志 tail 出来。

set -u            # 不用 -e：要让失败的测试继续跑完，最后统一汇总
ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

CC=${CC:-gcc}
JOBS=${JOBS:-4}

SAN=""
if [ "${WITH_SAN:-0}" = "1" ]; then
    SAN="-fsanitize=address,undefined"
fi
# ⚠️ `-D_POSIX_C_SOURCE=200809L` 必须与 Makefile 的 CSTD 保持一致。
#
# 少了它，`-std=c11` 走的是"严格 ISO C"分支 ⇒ <pthread.h> **不声明**
# `pthread_barrier_*`（那组 API 是 POSIX 2001 起才有的扩展）。
# 库代码碰巧没用到，但 test_wal_group_mt 用了 ⇒ 编译期 implicit declaration、
# 链接期找不到符号。**主核第一次在 Linux 上跑 test/run.sh 时撞到这一条** ——
# Windows/MinGW 的 winpthreads 头文件不按 _POSIX_C_SOURCE 裁剪，所以本地一直是绿的。
# 模态开关宏：默认 run.sh 跑 full，与 Makefile 的 MM=full 保持一致。
MM_FLAGS="-DDB_HAS_RELATIONAL=1 -DDB_HAS_BLOB=1 -DDB_HAS_GRAPH=1 \
-DDB_HAS_VECTOR=1 -DDB_HAS_FT=1 -DDB_HAS_GEO=1 -DDB_HAS_TS=1 -DDB_HAS_DOC=1"
CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O1 -g -fno-strict-aliasing \
$SAN $MM_FLAGS -Iinclude -Isrc"

# 线程库 + 数学库（v0.10 起有并发测试）
#
#   MinGW-w64：winpthreads 与 libm 都已经在默认库里 ⇒ 不需要额外参数
#   Linux    ：pthread 与 libm 都是独立的库 ⇒ 两个都要显式链接
#              （libm 是因为 bm25.c / mbr.c / rtree.c / json_scalar.c 用了数学函数）
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) LDFLAGS="" ;;
    *)                   LDFLAGS="-lpthread -lm" ;;
esac

# 测试单独编译，默认复用 Makefile 当前的对象目录。
# 通过 OBJ_DIR=build_mm_<profile> 调用时，测试就会链同一 profile 的对象。
OBJ_DIR=${OBJ_DIR:-build}
mkdir -p "$OBJ_DIR" test/tmp "$OBJ_DIR/test-logs"
# 排除 bench/ —— 基准程序自己带 main，链进来会重复定义。
OBJS=$(find "$OBJ_DIR" -name '*.o' ! -name 'main.o' ! -path "$OBJ_DIR/bench/*" 2>/dev/null || true)

# ==================== 单个测试：编译 → 运行 → 记结果 ====================
#
# 结果写面 <name>.rc（一个字节，内容是退出码）：
#   0 = PASS，1 = 编译失败，2 = 运行失败
# 三种结果分开，汇总时才能区分"编不过"和"断言没过"。
run_one() {
    t=$1
    name=$(basename "$t" .c)
    log="$OBJ_DIR/test-logs/$name.log"
    rc="$OBJ_DIR/test-logs/$name.rc"

    # test_stmt_alloc 自带分配器计数：链接时把 malloc 家族重定向到
    # 它内部的 __wrap_* 实现。这是**唯一**需要额外链接参数的文件，
    # 所以按名字分支，不给其他测试加负担。
    extra=""
    case "$name" in
        test_stmt_alloc)
            extra="-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free"
            ;;
    esac

    if ! $CC $CFLAGS -o "$OBJ_DIR/$name" "$t" $OBJS $LDFLAGS $extra >"$log" 2>&1; then
        echo 1 > "$rc"
        printf '  %-34s [COMPILE FAIL]\n' "$name"
        return
    fi
    if "./$OBJ_DIR/$name" >>"$log" 2>&1; then
        echo 0 > "$rc"
        printf '  %-34s [PASS]\n' "$name"
    else
        echo 2 > "$rc"
        printf '  %-34s [FAIL]\n' "$name"
    fi
}

found=0
list=""
for t in test/unit/test_*.c test/integration/test_*.c; do
    [ -e "$t" ] || continue
    found=1
    list="$list $t"
done

if [ "$found" -eq 0 ]; then
    echo "（没有找到测试文件）"
    exit 1
fi

total=0
for t in $list; do total=$((total + 1)); done
printf '并行 %d 路，共 %d 个测试：\n' "$JOBS" "$total"

pass=0
cfail=0
rfail=0
failed_names=""

# 清掉上次留下的 .rc，否则「这轮没跑到」会被误读成「上轮的结果」
rm -f "$OBJ_DIR"/test-logs/*.rc

# ==================== 计时敏感的测试单独串行 ====================
#
# 这几个的判据是**毫秒级的时序关系**，并行时 CPU/IO 争抢会让比值漂移：
#   test_ckpt_recovery_time  用 clock() 比 A/B/C 三种路径的耗时比值
#   test_catalog_lock        gate_wait 的 200ms/300ms 负向断言 + 2s 正向断言
#
# 串行跑它们只是多花几秒，但换掉了一类偶发红灯 —— 偶发红灯比慢更讨厌：
# 它会让"测试挂了"这件事失去信息量。
SERIAL_TESTS="test_ckpt_recovery_time test_catalog_lock"

is_serial() {
    for s in $SERIAL_TESTS; do
        [ "$1" = "$s" ] && return 0
    done
    return 1
}

batch=0
parallel_list=""
serial_list=""
for t in $list; do
    name=$(basename "$t" .c)
    if is_serial "$name"; then
        serial_list="$serial_list $t"
    else
        parallel_list="$parallel_list $t"
    fi
done

for t in $parallel_list; do
    ( run_one "$t" ) &
    batch=$((batch + 1))
    if [ "$batch" -ge "$JOBS" ]; then
        wait            # 无参 wait 等所有子进程 —— 就是"等这一批"
        batch=0
    fi
done
# 最后一批不满 JOBS 个
if [ "$batch" -gt 0 ]; then
    wait
fi

# 计时敏感的放最后，一个一个跑（此时 CPU 已空）
if [ -n "$serial_list" ]; then
    printf '  （以下 %d 个计时敏感，串行跑）\n' "$(echo $serial_list | wc -w)"
    for t in $serial_list; do
        run_one "$t"
    done
fi

# ==================== 汇总 ====================
for t in $list; do
    name=$(basename "$t" .c)
    case "$(cat "$OBJ_DIR/test-logs/$name.rc" 2>/dev/null || echo 9)" in
        0) pass=$((pass + 1)) ;;
        1) cfail=$((cfail + 1)); failed_names="$failed_names $name" ;;
        2) rfail=$((rfail + 1)); failed_names="$failed_names $name" ;;
        *) rfail=$((rfail + 1)); failed_names="$failed_names $name"; printf '  %-34s [NO RESULT]\n' "$name" ;;
    esac
done

printf '\n%d 个测试：%d 通过' "$total" "$pass"
[ "$cfail" -gt 0 ] && printf '，%d 编译失败' "$cfail"
[ "$rfail" -gt 0 ] && printf '，%d 断言失败' "$rfail"
printf '\n'

if [ -n "$failed_names" ]; then
    echo
    echo "失败详情（每个测试日志的最后 30 行）："
    for name in $failed_names; do
        rc=$(cat "$OBJ_DIR/test-logs/$name.rc" 2>/dev/null || echo 9)
        case "$rc" in
            1) printf -- '--- %s [COMPILE FAIL] (%s) ---\n' "$name" "$OBJ_DIR/test-logs/$name.log" ;;
            2) printf -- '--- %s [FAIL] (%s) ---\n' "$name" "$OBJ_DIR/test-logs/$name.log" ;;
            *) printf -- '--- %s [NO RESULT] (%s) ---\n' "$name" "$OBJ_DIR/test-logs/$name.log" ;;
        esac
        tail -30 "$OBJ_DIR/test-logs/$name.log" 2>/dev/null
        echo
    done
    exit 1
fi

exit 0
