#!/bin/sh
# 无 make 环境下的构建脚本（MinGW / Git Bash 常见情况）
#
# 用法：
#   sh build.sh           正常编译，产出 build/db.exe
#   sh build.sh san       开 ASan + UBSan 重编
#   sh build.sh clean

set -e
cd "$(dirname "$0")" || exit 1

CC=${CC:-gcc}
CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
-fno-strict-aliasing -Iinclude -Isrc"

case "${1:-}" in
clean)
    rm -rf build db db.exe
    echo "已清理"
    exit 0
    ;;
san)
    CFLAGS="$CFLAGS -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined"
    echo "（san 模式需要 MinGW 带 libasan；若链接失败就退回普通模式）"
    ;;
*)
    CFLAGS="$CFLAGS -O2 -g"
    ;;
esac

mkdir -p build
OBJS=""
for src in $(find src -name '*.c'); do
    obj="build/${src#src/}"
    obj="${obj%.c}.o"
    mkdir -p "$(dirname "$obj")"
    echo "  CC  $src"
    $CC $CFLAGS -c "$src" -o "$obj"
    OBJS="$OBJS $obj"
done

$CC $CFLAGS -o build/db.exe $OBJS
echo "✓ 构建完成：build/db.exe"

# 基准与参照工具（不进主二进制）
#   vec_bench    向量模态的验收基准
#   graph_bench  图模态的性能基准
#   hnswlib_ref  用 hnswlib（业界事实标准）算同一个数据集做对照
#
# 【每个基准是独立的可执行文件】
# 它们各自带 main —— 链到一起会 multiple definition of main。
CXX=${CXX:-g++}

mkdir -p build/bench
for src in $(find bench -name '*.c'); do
    obj="build/bench/${src#bench/}"
    obj="${obj%.c}.o"
    mkdir -p "$(dirname "$obj")"
    $CC $CFLAGS -Ibench -c "$src" -o "$obj"
done

# 基准要连主库的全部对象，但不能连 main.o —— 它自己带 main
LIB_OBJS=""
for obj in $OBJS; do
    case "$obj" in
        *main.o) ;;
        *) LIB_OBJS="$LIB_OBJS $obj" ;;
    esac
done

$CC $CFLAGS -o build/vec_bench.exe \
    build/bench/vec_bench.o build/bench/vec_bench_util.o $LIB_OBJS -lm
echo "✓ 构建完成：build/vec_bench.exe"

$CC $CFLAGS -o build/graph_bench.exe build/bench/graph_bench.o $LIB_OBJS
echo "✓ 构建完成：build/graph_bench.exe"

# hnswlib 是 header-only 的 C++ 库，直接用 g++ 编
$CXX -O2 -std=c++17 -I tools/third_party/hnswlib -o build/hnswlib_ref.exe tools/hnswlib_ref.cpp
echo "✓ 构建完成：build/hnswlib_ref.exe"
