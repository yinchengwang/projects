#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify-sdk-no-frozen.py —— SDK 不依赖任何已退役代码的机械门禁

背景：`engine/src/db/`（冻结轨）于 2026-11-18 退役。SDK 曾经靠它提供
内存上下文、读写锁、向量索引三样东西；这三样现已全部自持。
本脚本守住这条线：一旦有人重新引入对已退役代码的依赖，立刻红。

检查三项：
  1. SDK 源码与头文件里不得出现 `db/sql/` `db/parser/` `db/index/` 之类路径
  2. 不得出现 `faiss_hnsw` / `vector_index_selector` / `sql_engine` 这类已删目标名
  3. SDK 的 CMakeLists 链接行不得重新挂上这三个目标

用法：
    python scripts/verify-sdk-no-frozen.py
退出码 0 = 合规；1 = 有违规（逐条打印文件与行号）。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SRC_DIRS = ['engine/src/sdk', 'engine/include/sdk']
SUFFIXES = ('.c', '.h', '.cpp', '.hpp')

# 路径类：指向已退役目录
PATH_PAT = re.compile(r'db/sql/|db/parser/|db/index/|db/storage/|db/concurrency/')

# 名字类：已删的构建目标 / 已删的 API
NAME_PAT = re.compile(r'faiss_hnsw|vector_index_selector|sql_engine|mm_vector_index')

# CMake 链接行
CMAKE = 'engine/src/sdk/CMakeLists.txt'
LINK_PAT = re.compile(r'faiss_hnsw|vector_index_selector|sql_engine|mm_vector_index')

violations = []


def scan_sources():
    for d in SRC_DIRS:
        base = os.path.join(ROOT, d)
        if not os.path.isdir(base):
            continue
        for dp, _dn, fn in os.walk(base):
            for f in fn:
                if not f.endswith(SUFFIXES):
                    continue
                p = os.path.join(dp, f)
                rel = os.path.relpath(p, ROOT)
                with open(p, encoding='utf-8', errors='replace') as fh:
                    for i, line in enumerate(fh, 1):
                        if PATH_PAT.search(line):
                            violations.append('%s:%d: 引用已退役路径 —— %s' % (rel, i, line.strip()[:90]))
                        elif NAME_PAT.search(line):
                            violations.append('%s:%d: 引用已删目标名 —— %s' % (rel, i, line.strip()[:90]))


def scan_cmake():
    p = os.path.join(ROOT, CMAKE)
    if not os.path.isfile(p):
        return
    with open(p, encoding='utf-8', errors='replace') as fh:
        for i, line in enumerate(fh, 1):
            # 只看未注释的行
            code = line.split('#', 1)[0]
            if LINK_PAT.search(code):
                violations.append('%s:%d: 链接行重新挂上已删目标 —— %s' % (CMAKE, i, line.strip()[:90]))


scan_sources()
scan_cmake()

if violations:
    print('SDK 冻结轨依赖门禁: FAIL（%d 项）' % len(violations))
    for v in violations:
        print('  ' + v)
    sys.exit(1)

print('SDK 冻结轨依赖门禁: OK（%d 个源文件、%s 均无已退役依赖）'
      % (sum(1 for d in SRC_DIRS for dp, _dn, fn in os.walk(os.path.join(ROOT, d))
             for f in fn if f.endswith(SUFFIXES)), CMAKE))
sys.exit(0)
