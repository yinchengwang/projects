---
project: postgres
feature: gin-search
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/gin/ginget.c
    symbols: [gingetbitmap]
---

# GIN 搜索

## 一句话职责

GIN 搜索将查询键组合匹配到倒排项集合，并把候选 heap TID 以 bitmap 形式交给执行器。

## 机制是什么

GIN 查询把 operator class 产生的查询键映射到 entry tree，按查询语义组合各键对应的 posting TID；必要时返回 recheck 标志，表示索引匹配是候选而非 heap 行最终满足条件的证明。`gingetbitmap` 是 bitmap AM 扫描接口，最终将精确 TID 或 lossy page 加入 `TIDBitmap`。

GIN 不提供逐 tuple 的 `amgettuple` 路径，因为查询可能重复访问同一 TID，且 entry/posting 组合与 pending list 需要集合式处理。搜索必须先读 pending list，再读主索引：清理可能把条目从 pending list 移到主结构，若顺序相反可能在两次读取之间漏项。heap 可见性与 lossy 条件的最终复核由执行器/table-AM 完成。

## 怎么实现的

1. `gingetbitmap` 取得 `GinScanOpaque`，清理旧的 scan key 状态并调用 `ginNewScanKey`，将查询表达式转换为 GIN 搜索键及必要的组合树；不可满足的空查询直接返回 0。
2. 先调用 `scanPendingInsert` 遍历 pending list，并把符合查询的 TID 加入 bitmap。若并发 cleanup 同时将同一条目移入主索引，之后可能再次访问到同一 TID，但 bitmap 重复置位安全。
3. 随后 `startScan` 初始化主索引搜索，按键约束遍历 entry/posting 结构，通过 `scanGetItem` 逐步求得满足查询组合条件的候选 TID。
4. 每个结果带有 `recheck` 信息：精确结果以 tuple TID 加入 bitmap；如果索引结构只能确定某 heap page 上存在候选，则以 lossy page 形式加入，并要求 heap 阶段重检。
5. 统计返回候选数并结束。`TIDBitmap` 合并不同查询键带来的 TID，重复位不会重复产生 heap tuple；executor 再按 snapshot 与原始 qual 做可见性/条件检查。

## 关键取舍

- **源码事实：**实现按 pending list 后主索引的顺序扫描，允许相同 TID 被重复加入 bitmap；注释明确反序可能漏掉并发 cleanup 转移的项。**架构推论：**接受重复集合操作换取无遗漏的并发一致性；若先扫主索引后扫 pending，清理若恰在中间移动条目就会使该项既不在已扫主结构、也不在后扫 pending list。
- **源码事实：**GIN 扫描只提供 bitmap 输出；结果可标记 recheck，bitmap 还允许使用整页 lossy 表示。**架构推论：**集合式结果便于合并多键 posting list 并控制内存，但牺牲直接按索引序逐项返回的接口；若要求精确逐 TID 与顺序游标，复杂的键交集和 pending list 处理成本将落在 AM 上。
- **源码事实：**扫描返回的是候选 TID/页面而不是 heap tuple，最终由上层访问 heap。**架构推论：**索引逻辑与 MVCC 可见性相互解耦，允许 bitmap 扫描联合多个索引；代价是 heap 读取和 recheck 不能省略。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/gin/ginget.c` | `gingetbitmap` | 初始化查询键，按 pending-first 顺序扫描，并写入精确或 lossy TIDBitmap。 |

## 参考

- [PostgreSQL 文档：GIN 索引](https://www.postgresql.org/docs/current/gin.html) —— 介绍 GIN 倒排搜索、查询一致性与 recheck 语义。
- [PostgreSQL 官方源码：GIN bitmap 搜索](https://github.com/postgres/postgres/blob/master/src/backend/access/gin/ginget.c) —— 对照 pending list 优先扫描与 TIDBitmap 输出。

## 与主核 / AI 的关系

暂无直接对应。可借鉴 bitmap 将多个候选集合合并后再访问 heap 的执行接口；主核需自行定义 lossy 结果的重检与快照可见性协议。
