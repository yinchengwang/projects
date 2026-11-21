#!/bin/sh
# measure_build.sh —— 按 MM profile 度量构建规模
#
# 【为什么要有这个】
# 「模态裁剪能省多少」是个需要数字回答的问题。
# 没有数字 ⇒ 裁剪只是概念；有了数字 ⇒ 裁剪是工程事实。
#
# 用法：
#   sh tools/measure_build.sh              # 量全部 9 个 profile
#   sh tools/measure_build.sh vector ft    # 只量指定的
#
# 输出（每行一个 profile）：
#   profile  编译单元 目标文件 符号数 代码行数 构建秒数
#
# 【为什么「编译单元」和「目标文件」看起来一样】
# 每个 .c 编出一个 .o，一一对应 ⇒ 两列数字天然相同。
# 保留两列是因为它们的语义不同：一个问「编了几份源码」，
# 一个问「产出了几个目标文件」—— 将来若引入 unity build，两列就会分叉。
#
# 【「代码行数」为什么必须按 profile 算】
# `src/` 全量行数对每个 profile 都一样 ⇒ 那一列毫无区分度，
# 读者会误以为「裁剪没减少代码」。必须只数**该 profile 真正参与编译**的源文件。

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

# ⚠️ Windows 上 PATH 里没有 `make`，只有 `mingw32-make`
# （MinGW-w64 的 make 二进制叫 mingw32-make；`make` 是 msys2 那个）。
MAKE=${MAKE:-}
if [ -z "$MAKE" ]; then
    if command -v mingw32-make >/dev/null 2>&1; then
        MAKE=mingw32-make
    else
        MAKE=make
    fi
fi

# nm 在 Linux 上默认只列静态符号表；CI 可传 NMCMD="nm -D"
NMCMD=${NMCMD:-nm}

PROFILES=${*:-"full relational blob graph vector ft geo ts doc"}

printf '%-12s %8s %8s %8s %8s %8s\n' profile units objs symbols lines secs
printf '%-12s %8s %8s %8s %8s %8s\n' ------------- -------- -------- -------- -------- --------

for m in $PROFILES; do
    # ⚠️ 产物目录必须与 Makefile 的规则对齐：
    #   MM=full   ⇒ $(OBJ_BASE)
    #   其余      ⇒ $(OBJ_BASE)_mm_<m>
    case "$m" in
        full) objdir="build_measure" ;;
        *)    objdir="build_measure_mm_$m" ;;
    esac

    rm -rf "$objdir"

    # ⚠️ 必须传 OBJ_BASE，不能传 OBJ_DIR ——
    # OBJ_DIR 是**命令行变量**，在 GNU Make 里优先级最高，
    # `override OBJ_DIR := $(MM_OBJ_DIR)` 也压不住它
    # ⇒ 所有 profile 会共用同一个目录 ⇒ 裁剪产物与 full 产物混在一处。
    # OBJ_BASE 是本计划新引入的变量，不参与那条覆盖链。
    #
    # ⚠️ full 的可执行文件落在**仓库根**（db.exe / db），不在产物目录里 ——
    # 见 Makefile 的 TARGET 分支（full 保持与改动前逐字一致）。
    start=$(date +%s)
    $MAKE MM="$m" OBJ_BASE=build_measure all >/dev/null 2>&1
    rc=$?
    secs=$(( $(date +%s) - start ))

    if [ "$rc" -ne 0 ]; then
        printf '%-12s %8s\n' "$m" BUILD-FAIL
        rm -rf "$objdir"
        continue
    fi

    # 排除 main.o（它属于 REPL，不属于库）与 bench/
    objs=$(find "$objdir" -name '*.o' ! -name 'main.o' ! -path "$objdir/bench/*" 2>/dev/null | wc -l | tr -d ' ')
    units=$objs

    # 代码行数：把 .o 的路径映射回 .c，逐个 wc -l 累加。
    # 不数注释掉的空行之外的东西 —— 就是最朴素的物理行数，
    # 因为这一列要和「编译单元数」同一口径（都是"实际参与构建的源码"）。
    lines=0
    for o in $(find "$objdir" -name '*.o' ! -name 'main.o' ! -path "$objdir/bench/*" 2>/dev/null); do
        rel=${o#"$objdir"/}
        src="src/${rel%.o}.c"
        if [ -f "$src" ]; then
            n=$(wc -l < "$src" 2>/dev/null | tr -d ' ')
            lines=$(( lines + n ))
        fi
    done

    # 可执行文件位置：full 在仓库根，其余在自己的产物目录
    if [ "$m" = "full" ]; then
        if [ -f "db.exe" ]; then bin="db.exe"
        elif [ -f "db" ]; then bin="db"
        else bin=""
        fi
    else
        if [ -f "$objdir/db.exe" ]; then bin="$objdir/db.exe"
        elif [ -f "$objdir/db" ]; then bin="$objdir/db"
        else bin=""
        fi
    fi

    if [ -n "$bin" ]; then
        # 只数有定义的符号（T/t/W/w = 代码段），排除未定义的引用（U）
        syms=$($NMCMD "$bin" 2>/dev/null | grep -c ' [TtWw] ' || true)
        [ -z "$syms" ] && syms=0
    else
        echo "warn: profile $m 找不到可执行文件（符号数记 0）" >&2
        syms=0
    fi

    printf '%-12s %8s %8s %8s %8s %8s\n' "$m" "$units" "$objs" "$syms" "$lines" "$secs"

    rm -rf "$objdir"
    # full 的二进制在仓库根，也清掉（它有 .gitignore 覆盖，但别留脏）
    if [ "$m" = "full" ]; then
        rm -f db.exe db
    fi
done
