---
project: postgres
feature: hash-index-storage-and-operations
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/hash/hash.c
    symbols: [hashbuild, hashinsert, hashgettuple]
  - path: src/backend/access/hash/hashpage.c
    symbols: [_hash_getbuf, _hash_getinitbuf]
  - path: src/backend/access/hash/hashovfl.c
    symbols: [_hash_addovflpage, _hash_freeovflpage]
---

# Hash 索引操作与溢出页

## 一句话职责

Hash 索引把键映射到可扩展 bucket，并以溢出页链保存冲突 bucket 中的索引项。

## 机制是什么

Hash AM 只支持等值检索，索引 tuple 存储 hash code 与堆 TID；同 bucket 的项放在主 bucket 页及零到多个 overflow 页链中。metapage 保存 bucket 数量、mask 和 overflow 空间映射，扫描由 hash code 计算 bucket 后沿页链找候选项，因 hash code 可能碰撞，结果需要重新检查。

本机制负责 bucket 数据页、溢出页分配/释放与主要 build/insert/scan 入口，不负责键的原始值比较、堆可见性或一般排序。页访问 helper 只保证 buffer pin、读写锁及页类型校验；bucket 拆分及扫描期间的维护约束需与 cleanup lock、WAL 一起遵守。

## 怎么实现的

1. `hashbuild` 先按估计行数调用 `_hash_init` 建立 metapage、初始 buckets 与 bitmap page；当预估规模达到排序阈值时先 spool，再按 bucket 排序写入，降低散乱访问。
2. `hashinsert` 通过 `_hash_convert_tuple` 得到 hash code，组装索引 tuple 和堆 TID，再交给 `_hash_doinsert`。目标 bucket 由缓存的 metapage masks 映射，常用元数据缓存减少每次寻址都读 metapage。
3. `hashgettuple` 把 `xs_recheck` 设为 true，经 `_hash_first` / `_hash_next` 遍历目标 bucket 链；dead tuple 可延迟记录并在离开页时统一处理。
4. `_hash_getbuf` 用 `ReadBuffer` 读取已存在块，按参数加 buffer lock，再执行 `_hash_checkpage`；`_hash_getinitbuf` 则对已存在块 zero-and-lock 后初始化。二者均禁止 `P_NEW`，扩展索引需走新页分配路径。
5. bucket 需要扩展时 `_hash_expandtable` 持 metapage 写锁，并尝试对旧 bucket 获取 cleanup lock；新 bucket 主页准备好后记录拆分状态，再遍历旧链把属于新 bucket 的项搬过去，最后清除进行中标志并标记旧 bucket 待 split cleanup。
6. `_hash_addovflpage` 先锁 bucket tail，再锁 metapage 并检索 bitmap 的空位；找不到可复用页则扩展关系。critical section 中同时更新 bitmap/metapage、overflow 页 header、前页 `hasho_nextblkno` 并写一个 WAL 记录，随后按调用约定释放或保留 primary pin。
7. VACUUM 清理时 `_hash_freeovflpage` 把待保留项移入写页，修复双向链邻接指针、重置被释放页并清空 bitmap bit；这些变化与 WAL 一体化，避免恢复后重复项或链断裂。

## 关键取舍

- 源码事实：Hash AM 只保存 hash code，所以扫描总是有损并要求 recheck；相比保存原始键和比较排序，结构更紧凑且专注等值查找，代价是碰撞候选必须访问堆复核。
- 源码事实：overflow 页用 bitmap 复用而非删除后缩短文件；复用减少反复扩展文件，但分配需要维护 metapage、bitmap 与页链之间的一致性。
- 源码事实：overflow 分配尽量用单一 WAL 记录原子记录新页与 bucket 链接。拆成“先分配、后链接”两个记录可减少同时持锁的页面数，但若中间崩溃会遗留已分配却未链接的页。
- 源码事实：`_hash_expandtable` 获取不到旧 bucket cleanup lock 时可放弃本次 split；源码将此描述为无害，后续插入可再尝试。立即等待会阻塞扫描或并发维护，放弃则以暂时延迟扩容换取更短锁等待。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/hash/hash.c` | `hashbuild` | 初始化索引并选择直接写入或 bucket 排序 spool 构建 |
| `src/backend/access/hash/hash.c` | `hashinsert` | 转换键、形成索引 tuple 并调用底层插入 |
| `src/backend/access/hash/hash.c` | `hashgettuple` | 执行扫描推进并声明结果需堆端 recheck |
| `src/backend/access/hash/hashpage.c` | `_hash_getbuf` | 读取现存 hash 页、取得所需锁并校验类型 |
| `src/backend/access/hash/hashpage.c` | `_hash_getinitbuf` | 读取并初始化现存块，供从零重用页 |
| `src/backend/access/hash/hashovfl.c` | `_hash_addovflpage` | 分配/复用 overflow 页并原子接入 bucket 链 |
| `src/backend/access/hash/hashovfl.c` | `_hash_freeovflpage` | 从链中摘除溢出页、搬移项并释放 bitmap 配额 |

## 参考

- [PostgreSQL 文档：Hash 索引](https://www.postgresql.org/docs/current/hash-index.html) —— 说明 Hash 索引等值查找和 hash code 存储特征。
- [PostgreSQL 官方源码：Hash 溢出页管理](https://github.com/postgres/postgres/blob/master/src/backend/access/hash/hashovfl.c) —— 支撑 overflow 页分配、链维护及回收实现。

## 与主核 / AI 的关系

暂无直接对应。若主核采用哈希 bucket 与链页，需另行定义哈希碰撞复核、扩容一致性和回收协议，不能仅复制页链结构。
