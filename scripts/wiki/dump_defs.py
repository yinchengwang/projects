# -*- coding: utf-8 -*-
"""从 C 源码里抽取顶层函数定义名，供机制盘点时挑选真实符号。

用途：写机制页前必须确认「我要引用的符号确实定义在这个文件里」。
refs 校验是词边界匹配，不区分定义与调用，所以盘点阶段必须自建定义索引。

用法：
    python scripts/wiki/dump_defs.py --repo-root .cache/repos/sqlite --src-prefix src \\
        src/btree.c src/pager.c

    python scripts/wiki/dump_defs.py --repo-root .cache/repos/sqlite --src-prefix src \\
        --grep sqlite3Btree --limit 200
"""
from __future__ import annotations

import argparse
import io
import os
import re

# 行首返回类型 + 函数名( 的定义形态。覆盖三种常见写法：
#   int foo(void) {            sqlite / redis
#   static void foo(           redis
#   SQLITE_API int foo(        sqlite
#   int                         （换行参数表）foo(
_DEFINITION = re.compile(
    r"^(?:[A-Za-z_][A-Za-z0-9_]*\s+)*?"          # 返回类型（可含指针）
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\(",   # 函数名
    re.MULTILINE)

_KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "else", "do",
    "typedef", "struct", "union", "enum", "case", "defined", "static_assert",
}


def function_definitions(text: str) -> list[str]:
    names = []
    for match in _DEFINITION.finditer(text):
        name = match.group("name")
        if name in _KEYWORDS:
            continue
        # 过滤掉控制结构与常见误命中
        if name.startswith("SQLITE_") or name.isupper():
            continue
        names.append(name)
    return names


def resolve(repo_root: str, src_prefix: str, rel: str) -> str | None:
    rel = rel.replace("/", os.sep)
    candidates = []
    if src_prefix:
        prefix = src_prefix.replace("/", os.sep)
        candidates.append(os.path.join(repo_root, prefix, rel))
    candidates.append(os.path.join(repo_root, rel))
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="抽取 C 源码顶层函数定义名")
    parser.add_argument("--repo-root", required=True, help="源码仓根目录")
    parser.add_argument("--src-prefix", default="", help="源码子目录前缀，如 src")
    parser.add_argument("--grep", default="", help="只显示包含该子串的符号")
    parser.add_argument("--limit", type=int, default=0, help="每个文件最多显示多少个")
    parser.add_argument("files", nargs="+", help="相对源码根的文件路径")
    args = parser.parse_args(argv)

    for rel in args.files:
        full = resolve(args.repo_root, args.src_prefix, rel)
        if not full:
            print("%-32s 文件不存在" % rel)
            continue
        text = io.open(full, encoding="utf-8", errors="replace").read()
        names = sorted(set(function_definitions(text)))
        if args.grep:
            names = [name for name in names if args.grep in name]
        shown = names[: args.limit] if args.limit else names
        print("%-32s %3d 个定义" % (rel, len(names)))
        for name in shown:
            print("    ", name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
