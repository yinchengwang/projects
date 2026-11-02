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
