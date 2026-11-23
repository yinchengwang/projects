# -*- coding: utf-8 -*-
"""重建 postgres 的 _index.md：151 页、12 组的完整导航表。

导航表由 _manifest.yml 生成，不手写 —— 手写会在页数变动时漂移。
用法：
    py -3 rebuild_postgres_index.py            # 写 _index.md
    py -3 rebuild_postgres_index.py --dry-run  # 只打印导航表
"""
from __future__ import annotations

import argparse
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
POSTGRES_DIR = os.path.join(PROJECT_ROOT, "docs", "wiki", "projects", "postgres")
MANIFEST_PATH = os.path.join(POSTGRES_DIR, "_manifest.yml")
INDEX_PATH = os.path.join(POSTGRES_DIR, "_index.md")

GROUP_ORDER = [
    "A-存储与IPC",
    "B-事务与WAL",
    "C-堆表与元组",
    "D-索引族",
    "E-优化器",
    "F-执行器",
    "G-目录与缓存",
    "H-内存与排序",
    "I-复制与高可用",
    "J-服务端与命令面",
    "K-SQL处理与安全",
    "L-SQL类型与内建函数",
]

FRONT_MATTER = """---
project: postgres
page_type: overview
display_name: PostgreSQL
repo: https://github.com/postgres/postgres.git
license: PostgreSQL
language: C
stars: {stars}
stars_as_of: {stars_as_of}
refs:
  - path: src/backend/tcop/postgres.c
    symbols: [PostgresMain]
  - path: src/backend/optimizer/plan/planner.c
    symbols: [standard_planner]
  - path: src/backend/executor/execMain.c
    symbols: [ExecutorStart]
  - path: src/backend/access/transam/xlog.c
    symbols: [XLogInsertRecord, XLogFlush]
  - path: src/backend/storage/buffer/bufmgr.c
    symbols: [ReadBufferExtended]
---
"""

BODY_HEAD = """
# PostgreSQL

## 一句话职责

PostgreSQL 是提供关系型 SQL、事务、并发控制、持久化与扩展接口的开源数据库管理系统。

## 机制是什么

PostgreSQL 后端以进程承载客户端会话；SQL 解析、规划和执行分层完成查询处理。MVCC、WAL、锁及存储管理协作提供事务一致性与持久化。本文库按经审阅的机制单元组织：**12 个机制领域、共 {total} 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

页数不是按源码文件数折算的：本库 `src/backend/**` 有 904 个 `.c` 文件，而机制单元是逐条盘点后合并、拆分、移组得到的 {total} 个。文件名兜底的伪主题（如「xxx.c 模块」）在盘点阶段已被清除。

## 怎么实现的

后端会话接收协议消息并执行 SQL；优化器生成 Plan，执行器驱动表/索引访问方法。事务快照与提交状态判定版本可见性，WAL 为变更记录和崩溃恢复提供依据。共享缓冲池缓存关系页，smgr/md 将逻辑块映射到关系文件。进程槽、等待事件、共享内存和复制设施支撑并发及高可用。

下表的 12 个领域按「从存储到 SQL 面」的顺序排列：A 存储与 IPC → B 事务与 WAL → C 堆表与元组 → D 索引族 → E 优化器 → F 执行器 → G 目录与缓存 → H 内存与排序 → I 复制与高可用 → J 服务端与命令面 → K SQL 处理与安全 → L SQL 类型与内建函数。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

## 关键取舍

PostgreSQL 以多进程会话和明确的子系统接口换取隔离与可扩展性，代价是机制路径往往跨越多个实现层。MVCC 减少读写互阻，但要求维护事务状态、快照及版本回收；WAL 支持恢复，但写出顺序须满足日志先于数据页的持久化约束。

文档层面另有一条取舍：**机制粒度优先于文件粒度**。按文件切页能机械覆盖全部源码，但会产生大量「这个文件里有什么」的伪机制页；按机制切页需要人工盘点，代价是盘点成本与主观判断，收益是每一页都能回答「这个机制为什么这样设计、不负责什么」。本库选后者。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `src/backend/tcop/postgres.c` | `PostgresMain` | 后端会话循环 |
| `src/backend/optimizer/plan/planner.c` | `standard_planner` | 查询规划入口 |
| `src/backend/executor/execMain.c` | `ExecutorStart`、`ExecutorRun` | 执行器生命周期 |
| `src/backend/access/transam/xlog.c` | `XLogInsertRecord`、`XLogFlush` | WAL 插入与刷写 |
| `src/backend/storage/buffer/bufmgr.c` | `ReadBufferExtended` | 共享缓冲读入 |
| `src/backend/storage/smgr/smgr.c` | `smgropen` | 关系物理存储句柄 |

## 参考

- [PostgreSQL 官方文档：简介](https://www.postgresql.org/docs/current/intro-whatis.html)
- [PostgreSQL 官方文档：体系架构教程](https://www.postgresql.org/docs/current/tutorial-arch.html)
- [PostgreSQL 官方源码仓库](https://github.com/postgres/postgres)

## 与主核 / AI 的关系

本仓主数据库内核是从零实现的多模态数据库；PostgreSQL 页面用于机制对照，比较时应明确接口、事务语义、存储表示与恢复保证的差异。AI 生成的导航不能代替对指定 checkout 源码及版本的核对。
"""


def load_manifest():
    sys.path.insert(0, HERE)
    from wiki_manifest import load_manifest as _load

    return _load(MANIFEST_PATH)


def catalog_stars():
    """从 catalog 读 postgres 的 stars/stars_as_of；缺失时保持占位值。"""
    catalog_path = os.path.join(PROJECT_ROOT, "docs", "wiki", "catalog-projects.yml")
    sys.path.insert(0, HERE)
    from wiki_catalog import parse_catalog

    with io.open(catalog_path, encoding="utf-8") as stream:
        entries = parse_catalog(stream.read())
    for entry in entries:
        if entry.get("name") == "postgres":
            return entry.get("stars", 0), entry.get("stars_as_of", "1970-01-01")
    return 0, "1970-01-01"


def build_nav_table(manifest):
    lines = []
    by_group = {}
    for page in manifest["pages"]:
        by_group.setdefault(page["feature_group"], []).append(page)

    for group in GROUP_ORDER:
        pages = by_group.get(group, [])
        if not pages:
            continue
        lines.append("### %s（%d 篇）" % (group, len(pages)))
        lines.append("")
        lines.append("| 页文件 | 标题 |")
        lines.append("|---|---|")
        for page in pages:
            lines.append("| [`%s`](%s) | %s |" % (page["page_file"], page["page_file"], page["title"]))
        lines.append("")
    return "\n".join(lines).rstrip("\n")


def build_document(manifest, stars, stars_as_of):
    total = len(manifest["pages"])
    head = FRONT_MATTER.format(stars=stars, stars_as_of=stars_as_of)
    body = BODY_HEAD.format(total=total)
    nav = build_nav_table(manifest)
    nav_section = "## 机制导航（%d 篇 / 12 组）\n\n%s\n" % (total, nav)
    # 导航放在「怎么实现的」之后、「关键取舍」之前：读者先看体系，再按需跳转。
    marker = "\n## 关键取舍"
    return head + body.replace(marker, "\n" + nav_section + marker, 1)


def main(argv=None):
    parser = argparse.ArgumentParser(description="重建 postgres _index.md 导航表")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    manifest = load_manifest()
    stars, stars_as_of = catalog_stars()
    document = build_document(manifest, stars, stars_as_of)

    if args.dry_run:
        print(document)
        return 0

    with io.open(INDEX_PATH, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(document)
    print("已写出 %s（%d 页 / %d 组），stars=%s（%s）"
          % (INDEX_PATH, len(manifest["pages"]), len(GROUP_ORDER), stars, stars_as_of))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
