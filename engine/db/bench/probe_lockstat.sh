#!/bin/sh
# 一次性诊断脚本：带锁等待统计的构建
set -e
cd "$(dirname "$0")/.."
mkdir -p build_stat
CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O2 -g -Iinclude -Isrc -DDB_LOCKSTAT=1"
# 全量重编到 build_stat/
for f in $(find src -name '*.c' ! -name 'main.c'); do
    o="build_stat/$(echo "$f" | sed 's|/|_|g' | sed 's|\.c$|.o|')"
    gcc $CFLAGS -c -o "$o" "$f"
done
gcc $CFLAGS -Ibench -c -o build_stat/latch_bench.o bench/latch_bench.c
OBJS=$(find build_stat -name '*.o' ! -name '*main.o' ! -name 'latch_bench.o')
gcc -o build/latch_bench_stat.exe build_stat/latch_bench.o $OBJS -lpthread
echo "built build/latch_bench_stat.exe"
