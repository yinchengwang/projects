> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# R13 DB 索引系统 — 规格定义

## 概述

R13 聚焦 DB 索引系统，涵盖 BTree/Hash/全文/空间索引原理与 LSM-Tree 存储架构。

## 7 张卡规格

| 卡 ID | 产品 | 类型 | Ring | 核心概念 |
|-------|------|------|------|----------|
| btree_idx | MySQL | basic | B+树索引结构、唯一/非唯一索引、聚簇 vs 辅助索引、覆盖索引 |
| btree_impl | MySQL | intermediate | Page Directory、页分裂/合并、ICP、MRR |
| hash_idx | MySQL | basic | 线性哈希、可扩展哈希、链地址冲突解决 |
| fulltext | Elasticsearch | intermediate | 倒排索引、ngram 分词、BM25 评分 |
| spatial | PostgreSQL | intermediate | R-Tree MBR、最小面积插入、空间查询剪枝 |
| compaction | RocksDB | intermediate | Level/Tiered/Universal Compaction、写放大权衡 |
| lsm | RocksDB | advanced | MemTable/SSTable/WAL/Bloom Filter、读写路径 |

## 学习目标

- 理解 MySQL InnoDB B+Tree 索引的磁盘结构与查询优化
- 掌握 PostgreSQL 空间索引（R-Tree/GiST）与全文索引（GIN）原理
- 理解 LSM-Tree 的 Compaction 策略与性能权衡
- 能够对比不同索引类型的适用场景

## 工程对照

| 学习卡 | 工程源码 | 对照重点 |
|--------|----------|----------|
| btree_idx | `engineering/src/db/index/btree/` | BTree vs B+Tree、变长 key/value、持久化 |
| btree_impl | `engineering/src/db/index/btree/` | 页内二分、Top-down 分裂、借位 vs 合并 |
| hash_idx | `engineering/src/db/index/hash/` | 线性哈希实现、桶分裂 |
| fulltext | `engineering/src/db/index/fulltext/` | GIN 索引、WHITESPACE/CHINESE_MM 分词器、AND/OR/NOT 语法 |
| spatial | `engineering/src/db/index/rtree/` | rect_t vs mbr_t、模块化 5 文件、删除功能 |
| compaction | `engineering/src/db/index/compaction/` | Level Compaction 触发条件、回放过程 |
| lsm | `engineering/src/db/index/lsm/` | SkipList MemTable、SSTable 格式、Bloom Filter |

## 完成标准

- [x] 7 张卡 scaffold 就位（`learning/scaffold/db/` 下 7 个目录）
- [x] 每卡 4 文件：main.c + Makefile + README.md + NOTES.md（≥100 字工程对照）
- [x] 7 张卡 gcc 编译通过
- [x] `statuses.json` done count：68 → 75
- [x] 双轨 CMake 配置通过（工程轨原有问题 `-ldb_bm25` 不计入）
