> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# R35 Grokking 设计模式 — 规格文档

## 能力标识

- **ID**: `r35-grokking-patterns`
- **名称**: Grokking 设计模式
- **类型**: 学习轨 · 设计模式
- **标签**: `design-patterns`, `grok`, `GoF`

## 概述

设计模式（Design Patterns）是面向对象设计中针对常见问题的可复用解决方案。本规格覆盖 GoF 23 种经典设计模式（创建型/结构型/行为型）及混沌工程、反模式等工程实践。所有斯卡福德代码展示模式的 GoF 定义、UML 类图等价实现、适用场景及优缺点分析。

## 卡片清单

| # | ID | 目录 | 语言 | 主题 | ring |
|---|-----|------|------|------|------|
| 1 | monkey_testing | `monkey_testing/` | Python | 混沌测试/故障注入 | basic |
| 2 | stack_overflow | `stack_overflow/` | C | 堆栈溢出/尾递归 | basic |
| 3 | deadlock | `deadlock/` | C | 死锁/哲学家就餐 | basic |
| 4 | anti_patterns | `anti_patterns/` | Python | 反模式/代码异味 | basic |
| 5 | design_patterns | `design_patterns/` | Python | GoF 23 种模式全景 | intermediate |
| 6 | singleton | `pattern_singleton/` | Python | 单例模式 | basic |
| 7 | factory | `pattern_factory/` | Python | 工厂模式 | basic |
| 8 | observer | `pattern_observer/` | Python | 观察者模式 | basic |
| 9 | adapter | `pattern_adapter/` | Python | 适配器模式 | basic |
| 10 | facade | `pattern_facade/` | Python | 外观模式 | basic |
| 11 | command | `pattern_command/` | Python | 命令模式 | basic |
| 12 | strategy | `pattern_strategy/` | Python | 策略模式 | basic |
| 13 | state | `pattern_state/` | Python | 状态模式 | intermediate |
| 14 | template_method | `pattern_template/` | Python | 模板方法模式 | basic |
| 15 | proxy | `pattern_proxy/` | Python | 代理模式 | basic |
| 16 | composite | `pattern_composite/` | Python | 组合模式 | basic |

## 斯卡福德结构要求

每张卡包含 4 个文件：

```
<card-dir>/
├── main.py       # 演示代码（≥100 行）
├── Makefile      # 运行配置
├── README.md     # 卡片说明
└── NOTES.md      # 学习笔记（含工程对照 ≥100 字）
```

对于 C 卡：`main.c` 替代 `main.py`。

## 工程对照要求

每张卡的 NOTES.md 必须包含工程对照章节，关联 `engineering/` 轨中的对应实现：

| 卡片 | 工程对照参考 |
|------|-------------|
| monkey_testing | `engineering/src/db/core/wal.c` (crash recovery), `bufmgr.c` (Clock-Sweep) |
| stack_overflow | `engineering/src/db/buf/bufmgr.c` (memory mgmt), `src/ds/alloc/` (slab allocator) |
| deadlock | `engineering/src/db/txn/deadlock.c` (WFG deadlock detection) |
| anti_patterns | `engineering/test/` (Arrange-Act-Assert), `cmake/ProjectUtils.cmake` (DRY) |
| design_patterns | `engineering/include/db/storage_engine.h` (OCP/DIP) |
| singleton | `engineering/src/db/core/guc.c` (single config instance) |
| factory | `engineering/include/db/storage_engine.h` (register_engine pattern) |
| observer | `engineering/src/db/core/wal.c` (WAL event system) |
| adapter | `engineering/include/db/mm_storage.h` (storage engine interface) |
| facade | `engineering/src/db/core/db_server.c` (SQL interface facade) |
| command | `engineering/src/db/txn/` (transaction as command) |
| strategy | `engineering/include/db/mm_storage.h` (runtime engine selection) |
| state | `engineering/src/db/txn/` (transaction state machine) |
| template_method | `engineering/test/` (TEST macro), `cmake/ProjectUtils.cmake` |
| proxy | `engineering/src/db/buf/bufmgr.c` (buffer cache proxy) |
| composite | `engineering/src/db/btreeam/btreeam.c` (B+ tree composite pattern) |

## 数据注册

在 `items-registry.js` 中以 `product: "design-patterns"` 注册，在 `statuses.json` 中初始为 `todo`，完成后标记 `done`。

## 变更记录

| 日期 | 变更 |
|------|------|
| 2026-07-12 | 初始创建，16 张卡的斯卡福德 |
