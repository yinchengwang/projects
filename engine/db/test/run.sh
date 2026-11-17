#!/bin/sh
# 编译并运行 test/unit 与 test/integration 下的 test_*.c
#
# 默认不开 sanitizer（MinGW 常缺 libasan）；需要时：
#   WITH_SAN=1 sh test/run.sh

set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

CC=${CC:-gcc}
SAN=""
if [ "${WITH_SAN:-0}" = "1" ]; then
    SAN="-fsanitize=address,undefined"
fi
CFLAGS="-std=c11 -Wall -Wextra -Werror -O1 -g -fno-strict-aliasing \
$SAN -Iinclude -Isrc"

# 线程库（v0.10 起有并发测试）
#   MinGW-w64 的 winpthreads 已经在默认库里；Linux 要显式链接
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) LDFLAGS="" ;;
    *)                   LDFLAGS="-lpthread" ;;
esac

mkdir -p build test/tmp
# 排除 build/bench/ —— 基准程序自己带 main，链进来会重复定义。
# （bench 的对象是 2026-11-02 的步骤 6 加进来的；
#  一开始没排除，make bench 之后测试链接就会炸。）
OBJS=$(find build -name '*.o' ! -name 'main.o' ! -path 'build/bench/*' 2>/dev/null || true)

fail=0
found=0
for t in test/unit/test_*.c test/integration/test_*.c; do
    [ -e "$t" ] || continue
    found=1
    name=$(basename "$t" .c)

    # test_stmt_alloc 自带分配器计数：链接时把 malloc 家族重定向到
    # 它内部的 __wrap_* 实现。这是**唯一**需要额外链接参数的文件，
    # 所以按名字分支，不给其他测试加负担。
    extra=""
    case "$name" in
        test_stmt_alloc)
            extra="-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free"
            ;;
    esac

    printf '%s ... ' "$name"
    if $CC $CFLAGS -o "build/$name" "$t" $OBJS $LDFLAGS $extra; then
        if "./build/$name"; then
            echo " [PASS]"
        else
            echo " [FAIL]"
            fail=1
        fi
    else
        echo " [COMPILE FAIL]"
        fail=1
    fi
done

if [ "$found" -eq 0 ]; then
    echo "（没有找到测试文件）"
fi

exit "$fail"
