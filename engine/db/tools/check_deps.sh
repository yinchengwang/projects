#!/bin/sh
# 依赖方向检查：低层不许 include 高层。
# 这个脚本比任何架构文档都管用——它会在你手滑的那一刻拦住你。

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

fail=0

forbid() {
    dir=$1
    shift
    for bad in "$@"; do
        hits=$(grep -rn --include='*.c' --include='*.h' "#include \"[^\"]*${bad}" "$dir" 2>/dev/null)
        if [ -n "$hits" ]; then
            echo "✗ $dir 不允许依赖 $bad"
            echo "$hits" | sed 's/^/    /'
            fail=1
        fi
    done
}

# L0 谁都不许依赖
forbid src/common  storage/ index/ wal/ txn/ catalog/ query/ spi/ modalities/

# L1 只能依赖 common
forbid src/storage index/ txn/ catalog/ query/ spi/ modalities/
forbid src/wal     index/ txn/ catalog/ query/ spi/ modalities/

# L2 可以依赖 storage/wal
forbid src/index   query/ spi/ modalities/
forbid src/txn     query/ spi/ modalities/
forbid src/catalog query/ spi/ modalities/

# L3 可以依赖 index/txn/catalog
forbid src/query   modalities/

# ---- S9①：query/ 不许碰模态**实现**（模态的知识只能经 spi/ 的契约传进来）----
#
# 【为什么单列一条】
# 上面那条 `forbid src/query modalities/` 只能挡 `modalities/<m>/` 这个**目录前缀**。
# 但模态实现在 2026-11 之前并不是全都在 modalities/ 下：
#   blob   的实现住在 src/storage/blob*.c 与 src/spi/blob_am.c
#   relational 的堆实现在 src/storage/heap*.c（query 只能经 spi/heap_am.h 用）
# ⇒ 光挡目录前缀，这几条路径换个位置就漏了。
#
# ⚠️ **禁止词必须带路径口径，不能用裸单词** ——
# `forbid()` 是**子串匹配**（`#include "[^"]*${bad}`，`bad` 前面没有分隔符约束），
# 所以裸词 `heap` 会误伤**合法的** `#include "spi/heap_am.h"`
# （query 通过 SPI 契约访问堆，这正是它该做的事）。
# ⇒ 一律写成 `storage/heap` / `storage/blob` / `modalities/` 这种**带目录的**形式。
#
# ⚠️ 这份词表要随代码演进复核：每搬一次模态实现、每加一个模态，都该回来看一眼。
forbid src/query  modalities/ storage/heap storage/blob spi/blob_am spi/index_am

# ---- S9②：模态适配器（*_am.c）必须住在 src/modalities/<m>/ 下 ----
#
# spi/ 只放**契约**（storage_am.h / index_am.h / pushdown.h / registry.h），
# 不放实现。每个模态自己的 AM 适配器属于那个模态。
#
# ⚠️ **本规则当前是「预告」状态，不阻断** —— 现实的目录是这样的：
#     src/spi/heap_am.c / heap_am_batch.c / btree_am.c → relational 的实现
#     src/spi/blob_am.c                                 → blob 的实现
#   这 4 个文件要等 **W-C 的 C-2 / C-3**（目录搬家）才会移到
#     src/modalities/relational/ 与 src/modalities/blob/。
#   在搬家之前把这条设成阻断 ⇒ `make check` 恒红 ⇒ 门禁失去意义
#   （一个永远红的检查和一个永远绿的检查一样没用）。
#
# ⇒ 现在的行为：**只报告、不失败**，并明确写出「还差哪几个文件」。
#   等 C-2/C-5 落地后，把 MM_S9_ENFORCE 默认改成 1 就变成阻断。
#   要立刻验它能不能抓，跑：
#     MM_S9_ENFORCE=1 sh tools/check_deps.sh
S9_ENFORCE=${MM_S9_ENFORCE:-0}
bad_am=$(find src/spi -name '*_am.c' 2>/dev/null || true)
if [ -n "$bad_am" ]; then
    echo "⚠ S9② 待办：以下模态适配器还在 src/spi/（W-C 的 C-2/C-3 会搬走）："
    echo "$bad_am" | sed 's/^/    /'
    if [ "$S9_ENFORCE" = "1" ]; then
        echo "✗ S9② 已启用阻断：模态适配器实现不许留在 src/spi/"
        fail=1
    fi
fi

# L4 模态之间不许互相 include（这条决定三年后能不能加第 9 个模态）
for m in src/modalities/*/; do
    [ -d "$m" ] || continue
    for other in src/modalities/*/; do
        [ -d "$other" ] || continue
        [ "$m" = "$other" ] && continue
        base=$(basename "$other")
        hits=$(grep -rn --include='*.c' --include='*.h' "#include \"[^\"]*${base}/" "$m" 2>/dev/null)
        if [ -n "$hits" ]; then
            echo "✗ 模态 $m 不允许依赖模态 $base"
            echo "$hits" | sed 's/^/    /'
            fail=1
        fi
    done
done

if [ "$fail" -eq 0 ]; then
    echo "✓ 依赖方向正确"
fi
exit "$fail"
