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
    printf '%s ... ' "$name"
    if $CC $CFLAGS -o "build/$name" "$t" $OBJS $LDFLAGS; then
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
