# -*- coding: utf-8 -*-
"""从 Python 源码抽取定义索引（函数 / async 函数 / 类方法 / 嵌套定义）。

为什么不能复用 `dump_defs.py`：那份正则按 C 的「行首返回类型 + 名称 + 左括号」
匹配，Python 里没有返回类型，`def` 靠缩进分级，装饰器与 `async` 前缀也会改变
形态。混用会把 `foo(...)` 之类的调用当定义。

本模块只做词法层面的定义识别，不解析 AST 之外的语义；目的是让机制盘点能
「按文件顺序看到真实定义及其行号」，refs 里引用的符号必须出现在这份索引中。
"""
from __future__ import annotations

import ast
import os


def _qualify(stack, name):
    parts = [item for item in stack if item]
    parts.append(name)
    return ".".join(parts)


def definitions(source, qualified=False):
    """返回源文本中的定义名列表（按出现顺序）。

    `qualified=False` 只给名字；`qualified=True` 给出 `Class.method` 形式，
    顶层函数保持裸名。装饰器、嵌套函数、嵌套类都会被收集。
    """
    tree = ast.parse(source)
    results = []

    def walk(node, stack):
        for child in node.body:
            if isinstance(child, ast.ClassDef):
                results.append((_qualify(stack, child.name), child.lineno, "class"))
                walk(child, stack + [child.name])
            elif isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
                results.append((_qualify(stack, child.name), child.lineno, "function"))
                walk(child, stack + [child.name])
            elif isinstance(child, (ast.If, ast.Try, ast.With, ast.For, ast.While)):
                # 条件定义的函数仍属真实定义，按同一层级继续收集
                walk(child, stack)
            elif isinstance(child, ast.ExceptHandler):
                walk(child, stack)
            else:
                for grand in ast.iter_child_nodes(child):
                    if isinstance(grand, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                        walk_single(grand, stack)

    def walk_single(node, stack):
        if isinstance(node, ast.ClassDef):
            results.append((_qualify(stack, node.name), node.lineno, "class"))
            walk(node, stack + [node.name])
        else:
            results.append((_qualify(stack, node.name), node.lineno, "function"))
            walk(node, stack + [node.name])

    walk(tree, [])
    if qualified:
        return [name for name, _, _ in results]
    return [name.rsplit(".", 1)[-1] for name, _, _ in results]


def index_file(path):
    """索引单个 Python 文件，返回 [{name, line, kind}]。"""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        source = handle.read()
    tree = ast.parse(source)
    records = []

    def walk(node, stack):
        for child in node.body:
            if isinstance(child, ast.ClassDef):
                records.append({
                    "name": _qualify(stack, child.name),
                    "line": child.lineno,
                    "kind": "class",
                })
                walk(child, stack + [child.name])
            elif isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
                records.append({
                    "name": _qualify(stack, child.name),
                    "line": child.lineno,
                    "kind": "async function" if isinstance(child, ast.AsyncFunctionDef) else "function",
                })
                walk(child, stack + [child.name])
            else:
                for grand in ast.iter_child_nodes(child):
                    if isinstance(grand, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                        walk_single(grand, stack)

    def walk_single(node, stack):
        if isinstance(node, ast.ClassDef):
            records.append({"name": _qualify(stack, node.name), "line": node.lineno, "kind": "class"})
            walk(node, stack + [node.name])
        else:
            records.append({
                "name": _qualify(stack, node.name),
                "line": node.lineno,
                "kind": "async function" if isinstance(node, ast.AsyncFunctionDef) else "function",
            })
            walk(node, stack + [node.name])

    walk(tree, [])
    return records
