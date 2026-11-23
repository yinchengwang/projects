---
project: postgres
feature: spgist-access-maintenance
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/spgist/spgdoinsert.c
    symbols: [spgdoinsert, spgUpdateNodeLink]
  - path: src/backend/access/spgist/spgutils.c
    symbols: [spgGetCache, initSpGistState]
  - path: src/backend/access/spgist/spgscan.c
    symbols: [spgbeginscan, spgrescan]
  - path: src/backend/access/spgist/spgvacuum.c
    symbols: [spgbulkdelete, spgvacuumcleanup]
---

# SP-GiST 访问与维护

## 一句话职责

SP-GiST 以可扩展的分区树组织索引值，并维护内部节点到叶页链的链接、扫描状态及可回收空间。

## 机制是什么

SP-GiST 将结构策略交给 opclass：inner tuple 可有 prefix 和多个带 label 的 node，每个 node downlink 指向后续 tuple 或叶链。插入调用 choose、picksplit 等 support procedure 决定如何下降、分组和分裂；重复或冲突键可由 allTheSame 模式把旧项分散到多个子链。

扫描依据 opclass 的 inner/leaf consistent 函数遍历节点并返回候选 TID；ordered scan 用优先队列按距离组织搜索项。VACUUM 删除 dead heap TID 时必须保留外部 downlink 与扫描可见性语义，因而使用 redirect、dead 和 placeholder tuple 状态，而不是随意压紧 offset。它不负责通用 B-tree 排序、不替代 heap 可见性检查，也不负责 opclass 自身的值分区正确性。

## 怎么实现的

1. `spgGetCache` 首次访问时从 catalog 解析 key 类型并调用 opclass config，缓存 `SpGistCache.config`、输入/leaf/prefix/label 类型描述及 metapage last-used page 信息；`initSpGistState` 将其复制到操作态，并预备 redirect tuple 工作区与当前事务 redirect xid。
2. `spgdoinsert` 从 root/null root 取页，持 exclusive buffer lock 执行操作状态机。遇到 leaf chain 空间不足时，按轻量搬移或 `picksplit` 形成 inner tuple、node labels 和新的 leaf chains；必要时分配 leaf/inner 页，最后更新父 node 的 downlink。
3. `spgUpdateNodeLink` 按 node index 找到 inner tuple 中对应 node，并重写其 `t_tid` 为新 block/offset。若旧 tuple 不能原位扩展，插入路径保留 redirect（建索引时可用 placeholder），使并发扫描从旧位置继续到新位置。
4. `spgbeginscan` 初始化扫描描述符、临时与 traversal memory context，加载 inner/leaf consistent support procedure；`spgrescan` 复制并预处理 scan keys，区分 null 与非 null 搜索并重建起始工作队列。
5. scan walk 对 inner tuple 调用一致性函数决定要入队的 child downlink，对 live leaf 调用 leaf consistent；redirect 会把搜索转到目标 tuple，dead chain 终止。非 ordered 扫描按需收集页结果，ordered scan 以 pairing heap 按距离优先推进。
6. `spgbulkdelete` 通过 `spgvacuumscan` 扫描页面，用 callback 判定 heap TID 是否删除。链内被删项转 dead/placeholder，必要时把存活首项移到链头并重接 next offset；新建 redirect 目标进入 pending list 递归检查。
7. `spgvacuumcleanup` 若没有前序 bulkdelete，则执行不删除 live item 的清理扫描，仍可使过期 redirect 变 placeholder、尾部 placeholder 释放 offset/page，并更新 FSM 和 last-used page cache。

## 关键取舍

- 源码事实：扩展 leaf chain 时不能改变链首 downlink，插入可把新项放在首项之后；搬迁整条小链时将旧头替换成 redirect。直接删除并重排 offset 会让尚未更新的 parent downlink 指错位置，也可能让并发扫描遗漏或重复。
- 源码事实：VACUUM 只删除链中已判定 dead 的项，保留内部 offset 稳定性；尾部 placeholder 可安全删除，中间 placeholder 则保留到未来复用。这样以少量空间暂存换取 downlink 稳定，完全压紧需要修改外部链接并增加维护复杂度。
- 源码事实：`SpGistGetBuffer` 的 last-used cache 命中需条件锁并再次核验页类型和空闲空间；锁被占或缓存过时则分配新页。比等待热门缓存页更偏向并发进度，但偶尔多分配页面，后续由 VACUUM/FSM 帮助回收。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/spgist/spgdoinsert.c` | `spgdoinsert` | 驱动按 opclass 决策下降、添加节点、搬移或分裂的插入状态机 |
| `src/backend/access/spgist/spgdoinsert.c` | `spgUpdateNodeLink` | 更新 inner tuple 某个 node 的 block/offset downlink |
| `src/backend/access/spgist/spgutils.c` | `spgGetCache` | 初始化并缓存 opclass 配置与类型信息 |
| `src/backend/access/spgist/spgutils.c` | `initSpGistState` | 从 relation cache 形成一次操作所需状态 |
| `src/backend/access/spgist/spgscan.c` | `spgbeginscan` | 建立 scan opaque、上下文及一致性函数状态 |
| `src/backend/access/spgist/spgscan.c` | `spgrescan` | 预处理 quals 和 order-by 状态并重置遍历队列 |
| `src/backend/access/spgist/spgvacuum.c` | `spgbulkdelete` | 执行按 heap TID callback 清理的全页扫描 |
| `src/backend/access/spgist/spgvacuum.c` | `spgvacuumcleanup` | 清理 redirect/placeholder、FSM 并返回维护统计 |

## 参考

- [PostgreSQL 文档：SP-GiST 索引](https://www.postgresql.org/docs/current/spgist.html) —— 说明分区树访问方法与 opclass 支持接口。
- [PostgreSQL 官方源码：SP-GiST 插入](https://github.com/postgres/postgres/blob/master/src/backend/access/spgist/spgdoinsert.c) —— 支撑节点链接、搬移和 picksplit 过程。
- [PostgreSQL 官方源码：SP-GiST VACUUM](https://github.com/postgres/postgres/blob/master/src/backend/access/spgist/spgvacuum.c) —— 支撑 dead/redirect/placeholder 生命周期及清理。

## 与主核 / AI 的关系

暂无直接对应。SP-GiST 的分区策略和 redirect 生命周期可作为主核索引节点演进参考，但应按主核页格式、锁和恢复协议重新实现。
