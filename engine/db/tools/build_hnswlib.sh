#!/bin/sh
# build_hnswlib.sh — 取 hnswlib 的头文件（header-only 的 C++ 库）
#
# 为什么需要这个脚本：
#
#   A3 要求"对照 hnswlib"。但本机 pip 装不上 hnswlib ——
#   1) 该平台没有预编译 wheel
#   2) 从源码编译时 setup.py 要求 MSVC（hnswlib 的 Python 绑定）
#
#   而 hnswlib 的**核心是纯头文件**（MIT 协议），
#   只要 g++ 就能用，不需要 Python、不需要 pybind11。
#   所以这里只取头文件，参照程序 tools/hnswlib_ref.cpp 直接调它的 C++ API。
#
#   （顺带记一笔：pybind11 3.1.0 编译 hnswlib 0.8.0 的绑定能过，
#    但 add_items 一定段错误 —— 绑定层与 pybind11 3.x 不兼容。
#    绕开绑定层反而更稳。）
#
# 用法:
#   sh tools/build_hnswlib.sh
#
# 产物: tools/third_party/hnswlib/*.h

set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

VERSION=0.8.0
DEST=tools/third_party/hnswlib
WORK=${TMPDIR:-/tmp}/hnswlib-build

if [ ! -f "$DEST/hnswlib.h" ]; then
    echo "取 hnswlib $VERSION 的源码..."
    mkdir -p "$WORK"
    cd "$WORK" || exit 1

    # PyPI 的 sdist 里没有 hnswlib.py（那是 GitHub 仓库里的），
    # 但我们只要 C++ 头文件，所以 sdist 够用。
    if [ ! -f "hnswlib-$VERSION.tar.gz" ]; then
        curl -fsSL -o "hnswlib-$VERSION.tar.gz" \
            "https://files.pythonhosted.org/packages/source/h/hnswlib/hnswlib-$VERSION.tar.gz" \
            || curl -fsSL -o "hnswlib-$VERSION.tar.gz" \
            "https://pypi.org/packages/source/h/hnswlib/hnswlib-$VERSION.tar.gz"
    fi

    rm -rf "hnswlib-$VERSION"
    tar xzf "hnswlib-$VERSION.tar.gz"

    mkdir -p "$ROOT/$DEST"
    cp "hnswlib-$VERSION"/hnswlib/*.h "$ROOT/$DEST/"
    cp "hnswlib-$VERSION"/LICENSE "$ROOT/$DEST/"
    echo "✓ 头文件已放到 $DEST"
else
    echo "✓ $DEST 已有 hnswlib 头文件，跳过"
fi

echo
echo "接下来编译参照程序："
echo "  g++ -O2 -std=c++17 -I $DEST -o build/hnswlib_ref.exe tools/hnswlib_ref.cpp"
echo "（或直接 make bench）"
