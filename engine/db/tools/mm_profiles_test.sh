#!/bin/sh
# mm_profiles.mk 的正确性测试。
#
# mm_profiles.mk 是 8 个 profile 的唯一真相源：
#   - 集合错了       ⇒ profile 打开/关闭错误模态；
#   - 文件清单错了   ⇒ make 裁剪时漏编或链接失败；
#   - 依赖名错了     ⇒ 闭包计算失去意义。
#
# 这里不用 `$(shell)`，也不用 GNU make 的 --eval 配方技巧：
# Windows 下那条路径会在 cmd.exe / Git Bash 之间漂移。
# 每次校验都通过 stdin 喂一个极小的 wrapper makefile，
# MinGW make 与 Linux make 的行为一致。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

MAKE=${MAKE:-}
if [ -z "$MAKE" ]; then
    if command -v mingw32-make >/dev/null 2>&1; then
        MAKE=mingw32-make
    else
        MAKE=make
    fi
fi

fail=0

make_var() {
    var=$1
    tmp=${TMPDIR:-/tmp}/mm_profiles_$$.mk
    if command -v cygpath >/dev/null 2>&1; then
        include_root=$(cygpath -w "$ROOT")
    else
        include_root=$ROOT
    fi
    {
        printf 'include %s/tools/mm_profiles.mk\n' "$include_root"
        printf 'print:\n\t@echo $(%s)\n' "$var"
    } > "$tmp"
    got=$($MAKE -f "$tmp" print 2>/dev/null | tr -d '\r' | tail -1)
    rm -f "$tmp"
    printf '%s' "$got"
}

check_set() {
    profile=$1
    expected=$2
    got=$(make_var "MM_SET_$profile")
    if [ "$got" != "$expected" ]; then
        echo "FAIL MM_SET_$profile: 期望 [$expected] 实际 [$got]"
        fail=1
    fi
}

# ---- 断言 1：每个 profile 的模态集合符合预期 ----
check_set relational ""
check_set blob       "blob"
check_set graph      "graph"
check_set vector     "vector"
check_set ft         "ft"
check_set geo        "geo"
check_set ts         "ts"
check_set doc        "doc"
check_set full       "blob graph vector ft geo ts doc"

# ---- 断言 2：MM_FILES_<m> 列出的文件全部真实存在 ----
for m in relational blob graph vector ft geo ts doc; do
    files=$(make_var "MM_FILES_$m")
    for f in $files; do
        if [ ! -f "$f" ]; then
            echo "FAIL MM_FILES_$m 列了不存在的文件: $f"
            fail=1
        fi
    done
done

# ---- 断言 3：MM_DEP_<m> 的每个依赖都在 MM_ALL 里 ----
all=$(make_var MM_ALL)
for m in relational blob graph vector ft geo ts doc; do
    deps=$(make_var "MM_DEP_$m")
    for d in $deps; do
        case " $all " in
            *" $d "*) ;;
            *)
                echo "FAIL MM_DEP_$m 依赖了不在 MM_ALL 里的模态: $d"
                fail=1
                ;;
        esac
    done
done

if [ "$fail" -eq 0 ]; then
    echo "✓ mm_profiles.mk 全部断言通过"
fi
exit "$fail"
