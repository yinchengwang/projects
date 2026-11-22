#!/bin/sh
# check_a7.sh —— 文档模态接 SQL 时「执行器改动审计清单」的完整性门禁
#
# 【它防的是什么】
# PLAN-v1.6 第 5 步要求**先定义后实现**：执行器里允许出现文档相关代码的落点，
# 必须事先在 docs/MODALITIES.md §9.7 写死成清单。清单要是少写一处，
# 实现时就会"顺手"在别处加一个 if —— 事后没人能审计。
#
# 实测过的历史事实：§9.7 原文写「允许的落点只有两个」，
# 而实际上执行器有 5 处必须动（值层 / 两个求值入口 / lexer / parser 表达式 / parser DDL）。
# 少写的三处正是最容易破窗的地方。所以这条门禁盯的就是"五处都在"。
#
# 用法：
#   sh tools/check_a7.sh            # 只检查
#   退出码 0 = 合规
#
# ⚠️ 它检查的是**清单本身**，不是代码。代码侧的审计仍是人工跑
#    `grep -in json src/query/` 并逐行核对（见 §9.7 的审计命令）。

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

DOC=docs/MODALITIES.md

if [ ! -f "$DOC" ]; then
    echo "✗ 找不到 $DOC"
    exit 1
fi

# 取出 §9.7 那一段（到 §9.8 之前）
SECTION=$(sed -n '/^### 9\.7 执行器改动审计/,/^### 9\.8/p' "$DOC")

if [ -z "$SECTION" ]; then
    echo "✗ $DOC 里找不到 §9.7「执行器改动审计」章节"
    exit 1
fi

fail=0

need() {
    label=$1
    needle=$2
    if printf '%s' "$SECTION" | grep -q -- "$needle"; then
        :
    else
        echo "✗ §9.7 缺少落点 $label（找不到 '$needle'）"
        fail=1
    fi
}

need "① 通用值层"        "通用值层"
need "② eval_expr"       "exec_scan.c::eval_expr"
need "② eval_where"      "exec_eval.c::eval_where"
need "③ lexer"           "lexer"
need "④ parser 表达式"   "parser 表达式"
need "⑤ parser DDL"      "parser DDL"

# 审计纪律必须写着"逐行列出" —— 否则"命中在许可目录"就能糊过去
if printf '%s' "$SECTION" | grep -q "逐行"; then
    :
else
    echo "✗ §9.7 没有要求把 grep 命中逐行列进执行记录"
    fail=1
fi

# 反向检查：不许再出现"只有两个"这种已被推翻的口径
if printf '%s' "$SECTION" | grep -q "落点只有两个"; then
    echo "✗ §9.7 仍写着「允许的落点只有两个」—— 实测是 5 处，这句话是错的"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "✓ §9.7 审计清单完整（5 个落点 + 逐行审计要求）"
fi
exit "$fail"
