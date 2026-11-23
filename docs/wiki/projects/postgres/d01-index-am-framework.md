---
project: postgres
feature: index-am-framework
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/index/indexam.c
    symbols: [index_open, index_beginscan]
  - path: src/backend/access/index/genam.c
    symbols: [RelationGetIndexScan, BuildIndexValueDescription]
---

# 通用 IndexAM 扫描框架

## 一句话职责

通用索引访问层把关系打开、快照与扫描状态接到具体索引 AM 的回调上，并统一管理扫描资源的生命周期。

## 机制是什么

IndexAM 框架提供 AM 无关的入口和 `IndexScanDesc` 描述符；关系的 `rd_indam` 保存索引访问方法回调，框架按需调用 `ambeginscan`、`amrescan`、`amgettuple` 或 `amgetbitmap` 等接口。具体 AM 决定如何比较键、定位索引页以及返回 TID，通用层不实现 B-tree、GIN 等索引算法。

普通索引扫描要有调用者持有的 heap 与 index 关系锁，并带有合法快照。框架保存快照、扫描键数量、索引关系引用及可选 heap/table-AM 状态，结束时释放其引用和扫描内存。它不负责规划器是否选择某个索引，也不代替 heap/table AM 做可见性判定与取行；`BuildIndexValueDescription` 是错误信息辅助函数，会先检查行级安全与列读取权限，不能作为通用数据导出接口。

## 怎么实现的

1. `index_open` 通过 `relation_open` 打开 OID 对应关系，再验证其 relkind 为普通或分区索引；不存在或对象类型错误会报错。锁由传入的 lockmode 决定，`index_close` 可按要求释放锁，否则锁可延续至事务结束。
2. `index_beginscan` 要求有效 snapshot，并拒绝不适用于目标 heap 的 historic snapshot；进入内部初始化时检查正在 reindex 的关系，必要时对不支持 predicate lock 的 AM 加关系级谓词锁。
3. 内部入口增加 relcache reference count，再经 `rd_indam->ambeginscan` 取得 AM 的描述符。`RelationGetIndexScan` 分配并初始化通用 `IndexScanDescData`，其中包含 `indexRelation`、键计数、`keyData` / `orderByData` 和 AM 私有 `opaque` 等状态。
4. 框架将 snapshot、instrumentation 和 parallel scan 描述符写入 scan；有 heap relation 时设置 heap 及 index-only 请求，并调用 `table_index_scan_begin` 建立 table-AM 侧状态。bitmap 入口不传 heap relation，也不初始化该侧状态。
5. `index_rescan` 检查键数与开始扫描时一致，重置 table-AM scan state，再调用 AM 的 `amrescan`；扫描读取和键匹配细节留给 AM。
6. `index_endscan` 先结束 table-AM 状态，再调用 AM 的 `amendscan`，递减 relcache 引用，按临时快照标记注销 snapshot，最后由 `IndexScanEnd` 释放通用键数组与描述符。错误/中断由 PostgreSQL 错误清理框架回收资源。

## 关键取舍

- **源码事实：**框架把键缓冲区、snapshot、heap/table-AM 状态和 AM 私有 `opaque` 分层存放，并通过 `rd_indam` 回调分派；**架构推论：**这避免每个 AM 重复实现公共生命周期代码，但将扫描算法差异留在 AM 内。若通用层自行实现页遍历，就必须理解各 AM 不同的页格式和搜索语义，反而会耦合索引实现。
- **源码事实：**索引关系引用贯穿扫描，结束扫描时才递减；relation lock 则由调用者的锁模式及事务生命周期管理。**架构推论：**分别管理 relcache entry 与锁可让描述符使用期间元数据保持有效，同时允许调用方按上层锁协议决定锁何时释放；若把锁和引用强制绑定，便无法表达锁跨越 `index_close` 延续至事务末的路径。
- **源码事实：**`BuildIndexValueDescription` 在 RLS 开启或无权查看所有索引键列时返回 NULL；**架构推论：**错误诊断选择省略键值而非返回部分信息，避免错误消息泄漏敏感列值，代价是授权不足时诊断内容不完整。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/index/indexam.c` | `index_open` | 打开关系并验证其为索引关系。 |
| `src/backend/access/index/indexam.c` | `index_beginscan` | 校验 snapshot 后启动公共扫描初始化与 AM 回调。 |
| `src/backend/access/index/genam.c` | `RelationGetIndexScan` | 分配并初始化通用扫描描述符及键工作区。 |
| `src/backend/access/index/genam.c` | `BuildIndexValueDescription` | 生成受 RLS 和列 ACL 约束的索引键诊断字符串。 |

## 参考

- [PostgreSQL 文档：索引扫描类型](https://www.postgresql.org/docs/current/index-scanning.html) —— 说明索引扫描、bitmap 扫描及 heap 访问之间的边界。
- [PostgreSQL 官方源码：IndexAM 公共入口](https://github.com/postgres/postgres/blob/master/src/backend/access/index/indexam.c) —— 对照 AM 回调分派和扫描资源生命周期。

## 与主核 / AI 的关系

暂无直接对应。可借鉴其“稳定公共扫描描述符 + 可插拔 AM 回调”的边界，不应把具体索引算法混入公共生命周期管理。
