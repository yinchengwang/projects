# -*- coding: utf-8 -*-
"""重建任一库的 _index.md：概览正文 + 完整导航表。

导航表由 _manifest.yml 生成，不手写 —— 手写会在页数变动时漂移。

用法：
    py -3 rebuild_project_index.py --project redis          # 写 _index.md
    py -3 rebuild_project_index.py --project redis --dry-run # 只打印
    py -3 rebuild_project_index.py --all                    # 重建全部已配置库

各库的概览正文（职责、机制概览、取舍、参考）保存在本文件的 LIBRARIES 表里，
导航表部分则完全由清单驱动。
"""
from __future__ import annotations

import argparse
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
WIKI_ROOT = os.path.join(PROJECT_ROOT, "docs", "wiki", "projects")
CATALOG_PATH = os.path.join(PROJECT_ROOT, "docs", "wiki", "catalog-projects.yml")


class Library(object):
    """一个库的概览配置。"""

    def __init__(self, project, display_name, repo, license_name, language,
                 group_count, group_titles, summary, mechanism, tradeoffs,
                 landing_points, references, relation, stars_key=None):
        self.project = project
        self.display_name = display_name
        self.repo = repo
        self.license = license_name
        self.language = language
        self.group_count = group_count
        self.group_titles = group_titles
        self.summary = summary
        self.mechanism = mechanism
        self.tradeoffs = tradeoffs
        self.landing_points = landing_points
        self.references = references
        self.relation = relation
        self.stars_key = stars_key or project


LIBRARIES = {
    "postgres": Library(
        project="postgres",
        display_name="PostgreSQL",
        repo="https://github.com/postgres/postgres.git",
        license_name="PostgreSQL",
        language="C",
        group_count=12,
        group_titles=[
            "A 存储与 IPC", "B 事务与 WAL", "C 堆表与元组", "D 索引族",
            "E 优化器", "F 执行器", "G 目录与缓存", "H 内存与排序",
            "I 复制与高可用", "J 服务端与命令面", "K SQL 处理与安全",
            "L SQL 类型与内建函数",
        ],
        summary=(
            "PostgreSQL 是提供关系型 SQL、事务、并发控制、持久化与扩展接口的"
            "开源数据库管理系统。"
        ),
        mechanism=(
            "PostgreSQL 后端以进程承载客户端会话；SQL 解析、规划和执行分层完成"
            "查询处理。MVCC、WAL、锁及存储管理协作提供事务一致性与持久化。"
        ),
        tradeoffs=(
            "PostgreSQL 以多进程会话和明确的子系统接口换取隔离与可扩展性，"
            "代价是机制路径往往跨越多个实现层。MVCC 减少读写互阻，但要求维护"
            "事务状态、快照及版本回收；WAL 支持恢复，但写出顺序须满足日志先于"
            "数据页的持久化约束。\n\n"
            "文档层面另有一条取舍：**机制粒度优先于文件粒度**。按文件切页能"
            "机械覆盖全部源码，但会产生大量「这个文件里有什么」的伪机制页；"
            "按机制切页需要人工盘点，代价是盘点成本与主观判断，收益是每一页"
            "都能回答「这个机制为什么这样设计、不负责什么」。本库选后者。"
        ),
        landing_points=[
            ("src/backend/tcop/postgres.c", "PostgresMain", "后端会话循环"),
            ("src/backend/optimizer/plan/planner.c", "standard_planner", "查询规划入口"),
            ("src/backend/executor/execMain.c", "ExecutorStart, ExecutorRun", "执行器生命周期"),
            ("src/backend/access/transam/xlog.c", "XLogInsertRecord, XLogFlush", "WAL 插入与刷写"),
            ("src/backend/storage/buffer/bufmgr.c", "ReadBufferExtended", "共享缓冲读入"),
            ("src/backend/storage/smgr/smgr.c", "smgropen", "关系物理存储句柄"),
        ],
        references=[
            ("PostgreSQL 官方文档：简介", "https://www.postgresql.org/docs/current/intro-whatis.html"),
            ("PostgreSQL 官方文档：体系架构教程", "https://www.postgresql.org/docs/current/tutorial-arch.html"),
            ("PostgreSQL 官方源码仓库", "https://github.com/postgres/postgres"),
        ],
        relation=(
            "本仓主数据库内核是从零实现的多模态数据库；PostgreSQL 页面用于机制"
            "对照，比较时应明确接口、事务语义、存储表示与恢复保证的差异。"
            "AI 生成的导航不能代替对指定 checkout 源码及版本的核对。"
        ),
    ),

    "redis": Library(
        project="redis",
        display_name="Redis",
        repo="https://github.com/redis/redis.git",
        license_name="BSD-3-Clause",
        language="C",
        group_count=13,
        group_titles=[
            "A 事件循环与命令分派", "B 网络与客户端", "C 键空间与过期",
            "D 对象与编码", "E 底层数据结构", "F 数据类型", "G 持久化",
            "H 复制与高可用", "I 阻塞与通知", "J 事务脚本与模块",
            "K 安全", "L 特殊数据类型", "M 启动运维与后台任务",
        ],
        summary=(
            "Redis 是以内存为主要存储介质的键值数据库，兼具数据结构服务器、"
            "缓存与消息中间件三种角色。"
        ),
        mechanism=(
            "Redis 以单线程事件循环承载全部命令执行，自带的 ae 库把多路复用"
            "抽象成文件事件与时间事件两类回调。键空间由哈希表索引，值的编码"
            "随规模在紧凑与展开形态之间切换；过期既有惰性判定也有主动扫描，"
            "内存压力下按采样近似执行淘汰。持久化有 RDB 快照与 AOF 追加两条"
            "路径，复制基于命令流传播，集群与哨兵在此之上组织高可用。"
        ),
        tradeoffs=(
            "Redis 用单线程命令执行换取无锁的数据结构访问，代价是单个慢命令"
            "会阻塞整个实例，因此耗时操作被移到后台线程或交给 Lua 脚本在"
            "受限环境内完成。内存优先的存储模型换来低延迟，代价是必须同时"
            "提供持久化与淘汰策略来应对内存上限。\n\n"
            "文档层面沿用与 PostgreSQL 相同的取舍：**机制粒度优先于文件粒度**，"
            "每一页对应一个可独立验证的机制单元，而不是一个源文件。"
        ),
        landing_points=[
            ("src/server.c", "main", "服务端启动与主循环"),
            ("src/ae.c", "aeMain, aeProcessEvents", "事件循环"),
            ("src/networking.c", "readQueryFromClient, processInputBuffer", "客户端命令读取与解析"),
            ("src/db.c", "lookupKey, dbAdd, dbGenericDelete", "键空间读写与删除"),
            ("src/object.c", "createObject, tryObjectEncoding", "对象创建与编码转换"),
            ("src/rdb.c", "rdbSave, rdbLoad", "RDB 快照生成与载入"),
            ("src/aof.c", "feedAppendOnlyFile, flushAppendOnlyFile", "AOF 追加与刷盘"),
            ("src/cluster_legacy.c", "clusterInit, clusterProcessPacket", "集群总线"),
        ],
        references=[
            ("Redis 官方文档：命令参考", "https://redis.io/docs/latest/commands/"),
            ("Redis 官方文档：持久化", "https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/"),
            ("Redis 官方文档：客户端处理", "https://redis.io/docs/latest/develop/reference/clients/"),
            ("Redis 官方源码仓库", "https://github.com/redis/redis"),
        ],
        relation=(
            "本仓主数据库内核是从零实现的多模态数据库；Redis 页面用于机制"
            "对照，比较时应明确存储模型、并发模型与持久化保证的差异——尤其"
            "是「单线程命令执行」与本仓内核的并发模型并不等价。"
            "AI 生成的导航不能代替对指定 checkout 源码及版本的核对。"
        ),
    ),

    "mysql-server": Library(
        project="mysql-server",
        display_name="MySQL Server",
        repo="https://github.com/mysql/mysql-server.git",
        license_name="GPL-2.0",
        language="C++",
        group_count=14,
        group_titles=[
            "A 连接与命令分派", "B 解析与词法", "C 优化器与执行计划",
            "D 执行器与行迭代", "E 临时表与排序", "F 存储引擎接口",
            "G InnoDB 索引与页", "H InnoDB 缓冲池", "I InnoDB 日志与恢复",
            "J InnoDB 事务与锁", "K InnoDB 行操作与可见性", "L 二进制日志",
            "M 复制与 GTID", "N 权限与认证",
        ],
        summary=(
            "MySQL Server 是以 InnoDB 为默认存储引擎的开源关系型数据库"
            "管理系统，兼作客户端-服务器协议的服务端。"
        ),
        mechanism=(
            "MySQL Server 以前台进程承载连接，命令经分派后进入解析、计划与执行"
            "三层。解析阶段由词法与文法分析产出解析树，再转换为可求值的 Item；"
            "优化器在候选索引与连接顺序之间按代价取舍，产出访问路径；执行器把"
            "访问路径落到行迭代器上驱动取行。InnoDB 侧以 B-tree 组织聚簇索引与"
            "二级索引，缓冲池缓存页，redo 日志保证崩溃恢复，MVCC 与行锁共同"
            "支撑事务隔离。binlog 记录变更并支撑复制，GTID 提供复制定位。"
        ),
        tradeoffs=(
            "MySQL 用 handler 接口把 server 层与存储引擎解耦，代价是引擎必须"
            "适配一套约定良好的回调面，InnoDB 之外的引擎实现成本不低。优化器"
            "以代价模型换取可预测的查询计划，但统计信息陈旧时可能选错路径。\n\n"
            "文档层面沿用与 PostgreSQL 相同的取舍：**机制粒度优先于文件粒度**，"
            "每一页对应一个可独立验证的机制单元，而不是一个源文件。"
        ),
        landing_points=[
            ("sql/sql_parse.cc", "do_command, dispatch_command", "命令分派"),
            ("sql/sql_class.cc", "THD::sql_parser", "会话线程对象"),
            ("sql/sql_optimizer.cc", "JOIN::optimize, JOIN::make_join_plan", "优化器编排与连接计划"),
            ("sql/sql_select.cc", "Sql_cmd_dml::execute, JOIN::exec", "查询执行入口与连接执行"),
            ("sql/handler.cc", "ha_open, handler::ha_write_row", "存储引擎接口"),
            ("storage/innobase/btr/btr0cur.cc", "btr_cur_search_to_nth_level", "B-tree 游标定位"),
            ("storage/innobase/buf/buf0buf.cc", "buf_page_get_gen, buf_pool_get", "缓冲池取页"),
            ("storage/innobase/trx/trx0trx.cc", "trx_commit_for_mysql", "事务提交"),
            ("sql/log_event.cc", "Log_event::write", "binlog 事件写入"),
            ("sql/rpl_replica.cc", "exec_relay_log_event, start_slave", "复制应用"),
        ],
        references=[
            ("MySQL 官方文档：8.0 参考手册", "https://dev.mysql.com/doc/refman/8.0/en/"),
            ("MySQL 官方文档：InnoDB 内部机制", "https://dev.mysql.com/doc/refman/8.0/en/innodb-internal-architecture.html"),
            ("MySQL 官方文档：复制", "https://dev.mysql.com/doc/refman/8.0/en/replication.html"),
            ("MySQL 官方源码仓库", "https://github.com/mysql/mysql-server"),
        ],
        relation=(
            "本仓主数据库内核是从零实现的多模态数据库；MySQL 页面用于机制"
            "对照，比较时应明确 SQL 层与存储层的接口边界、锁模型与日志"
            "语义——MySQL 的 binlog 与 InnoDB redo 处在不同层次，不能混谈。"
            "AI 生成的导航不能代替对指定 checkout 源码及版本的核对。"
        ),
        stars_key="mysql",
    ),

    "sqlite": Library(
        project="sqlite",
        display_name="SQLite",
        repo="https://github.com/sqlite/sqlite.git",
        license_name="Public Domain",
        language="C",
        group_count=14,
        group_titles=[
            "A 连接与生命周期", "B 解析与语义解析", "C 表达式与代码生成",
            "D 查询编译", "E VDBE 执行", "F B-tree 与记录格式",
            "G Pager 与回滚日志", "H WAL", "I 页缓存", "J VFS 与内存管理",
            "K 约束与索引维护", "L 函数", "M 事务与并发", "N DDL 与 PRAGMA",
        ],
        summary=(
            "SQLite 是进程内嵌入、无需服务端的开源 SQL 数据库引擎，以单文件"
            "承载整个关系数据库。"
        ),
        mechanism=(
            "SQLite 以库内嵌方式运行，调用方直接链接其代码。SQL 文本经分词、"
            "语法分析与名称解析后形成表达式树，查询编译阶段把语句转成虚拟"
            "机字节码程序；执行器逐条解释字节码，通过分页器把页读写落到"
            "文件，文件之上的页缓存管理脏页与回收。存储层是 B-tree，"
            "记录采用变长编码。事务以回滚日志或 WAL 实现，两种模式下的可见性"
            "规则不同。"
        ),
        tradeoffs=(
            "SQLite 以单文件与进程内嵌入换取部署简单，代价是并发写受限、"
            "无服务端的多连接协调；同一时刻只能有一个写事务（WAL 模式下读不"
            "阻塞写）。整库复制以文件级操作为主，牺牲细粒度事务合并换取实现"
            "简单。\n\n"
            "文档层面沿用与 PostgreSQL 相同的取舍：**机制粒度优先于文件粒度**，"
            "每一页对应一个可独立验证的机制单元，而不是一个源文件。"
        ),
        landing_points=[
            ("src/main.c", "sqlite3_open, sqlite3_close", "连接建立与关闭"),
            ("src/tokenize.c", "sqlite3GetToken", "词法分析"),
            ("src/parse.y", "sqlite3Parser", "语法分析"),
            ("src/resolve.c", "sqlite3ResolveExprNames", "名称解析"),
            ("src/expr.c", "sqlite3ExprCodeTarget", "表达式代码生成"),
            ("src/select.c", "sqlite3Select", "SELECT 编译"),
            ("src/vdbe.c", "sqlite3VdbeExec", "字节码解释执行"),
            ("src/btree.c", "sqlite3BtreeOpen, sqlite3BtreeTableMoveto", "B-tree 游标"),
            ("src/pager.c", "sqlite3PagerGet, sqlite3PagerWrite", "分页器读页与加写标记"),
            ("src/wal.c", "sqlite3WalOpen, walCheckpoint", "WAL 打开与检查点"),
            ("src/os.c", "sqlite3OsRead, sqlite3OsWrite", "VFS 文件读写"),
        ],
        references=[
            ("SQLite 官方文档：文件格式", "https://www.sqlite.org/fileformat.html"),
            ("SQLite 官方文档：WAL 模式", "https://www.sqlite.org/wal.html"),
            ("SQLite 官方文档：隔离与并发", "https://www.sqlite.org/isolation.html"),
            ("SQLite 官方文档：源码结构", "https://www.sqlite.org/code.html"),
            ("SQLite 官方源码仓库", "https://github.com/sqlite/sqlite"),
        ],
        relation=(
            "本仓主数据库内核是从零实现的多模态数据库；SQLite 页面用于机制"
            "对照，比较时应明确分页器与存储层的分层边界、以及单文件嵌入式"
            "设计在并发与崩溃恢复上的取舍差异。"
            "AI 生成的导航不能代替对指定 checkout 源码及版本的核对。"
        ),
        stars_key="sqlite3",
    ),
}


def load_manifest(project):
    sys.path.insert(0, HERE)
    from wiki_manifest import load_manifest as _load

    return _load(os.path.join(WIKI_ROOT, project, "_manifest.yml"))


def catalog_stars(stars_key):
    """从 catalog 读 stars/stars_as_of；缺失时保持占位值。"""
    sys.path.insert(0, HERE)
    from wiki_catalog import parse_catalog

    with io.open(CATALOG_PATH, encoding="utf-8") as stream:
        entries = parse_catalog(stream.read())
    for entry in entries:
        if entry.get("name") == stars_key:
            return entry.get("stars", 0), entry.get("stars_as_of", "1970-01-01")
    return 0, "1970-01-01"


def build_nav_table(manifest, group_order):
    lines = []
    by_group = {}
    for page in manifest["pages"]:
        by_group.setdefault(page["feature_group"], []).append(page)

    for group in group_order:
        pages = by_group.get(group, [])
        if not pages:
            continue
        lines.append("### %s（%d 篇）" % (group, len(pages)))
        lines.append("")
        lines.append("| 页文件 | 标题 |")
        lines.append("|---|---|")
        for page in pages:
            lines.append("| [`%s`](%s) | %s |"
                         % (page["page_file"], page["page_file"], page["title"]))
        lines.append("")
    return "\n".join(lines).rstrip("\n")


def build_front_matter(library, stars, stars_as_of):
    lines = [
        "---",
        "project: %s" % library.project,
        "page_type: overview",
        "display_name: %s" % library.display_name,
        "repo: %s" % library.repo,
        "license: %s" % library.license,
        "language: %s" % library.language,
        "stars: %s" % stars,
        "stars_as_of: %s" % stars_as_of,
        "refs:",
    ]
    for path, symbols, _ in library.landing_points:
        lines.append("  - path: %s" % path)
        lines.append("    symbols: [%s]" % symbols)
    lines.append("---")
    return "\n".join(lines) + "\n"


def build_body(library, manifest, nav):
    total = len(manifest["pages"])
    group_count = library.group_count
    chain = " → ".join(library.group_titles)

    out = []
    out.append("")
    out.append("# %s" % library.display_name)
    out.append("")
    out.append("## 一句话职责")
    out.append("")
    out.append(library.summary)
    out.append("")
    out.append("## 机制是什么")
    out.append("")
    out.append(library.mechanism)
    out.append("")
    out.append("本文库按经审阅的机制单元组织：**%d 个机制领域、共 %d 篇机制页**，"
               "每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。"
               % (group_count, total))
    out.append("")
    out.append("## 怎么实现的")
    out.append("")
    out.append("下表的 %d 个领域按下面的顺序排列：%s。每一篇的具体边界、实现路径"
               "与取舍以该页正文为准，本页只做导航。" % (group_count, chain))
    out.append("")
    out.append("## 机制导航（%d 篇 / %d 组）" % (total, group_count))
    out.append("")
    out.append(nav)
    out.append("")
    out.append("## 关键取舍")
    out.append("")
    out.append(library.tradeoffs)
    out.append("")
    out.append("## 源码落点")
    out.append("")
    out.append("| 路径 | 符号 | 对应职责 |")
    out.append("|---|---|---|")
    for path, symbols, duty in library.landing_points:
        out.append("| `%s` | %s | %s |" % (path, symbols, duty))
    out.append("")
    out.append("## 参考")
    out.append("")
    for label, url in library.references:
        out.append("- [%s](%s)" % (label, url))
    out.append("")
    out.append("## 与主核 / AI 的关系")
    out.append("")
    out.append(library.relation)
    out.append("")
    return "\n".join(out)


def build_document(project, manifest, stars, stars_as_of):
    library = LIBRARIES[project]
    group_order = []
    for page in manifest["pages"]:
        if page["feature_group"] not in group_order:
            group_order.append(page["feature_group"])
    nav = build_nav_table(manifest, group_order)
    return build_front_matter(library, stars, stars_as_of) + build_body(library, manifest, nav)


def rebuild(project, dry_run=False):
    manifest = load_manifest(project)
    library = LIBRARIES[project]
    stars, stars_as_of = catalog_stars(library.stars_key)
    document = build_document(project, manifest, stars, stars_as_of)
    index_path = os.path.join(WIKI_ROOT, project, "_index.md")

    if dry_run:
        print(document)
        return 0

    with io.open(index_path, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(document)
    print("已写出 %s（%d 页 / %d 组），stars=%s（%s）"
          % (index_path, len(manifest["pages"]), library.group_count, stars, stars_as_of))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="重建 _index.md 概览与导航表")
    parser.add_argument("--project", help="库名（postgres / redis / mysql-server / sqlite）")
    parser.add_argument("--all", action="store_true", help="重建全部已配置库")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    if args.all:
        for project in LIBRARIES:
            rebuild(project, args.dry_run)
        return 0
    if not args.project:
        parser.error("需要 --project 或 --all")
    if args.project not in LIBRARIES:
        parser.error("未配置的库：%s（可选：%s）"
                     % (args.project, "、".join(sorted(LIBRARIES))))
    return rebuild(args.project, args.dry_run)


if __name__ == "__main__":
    raise SystemExit(main())
