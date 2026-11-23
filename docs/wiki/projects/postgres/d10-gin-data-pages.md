---
project: postgres
feature: gin-data-pages
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/gin/gindatapage.c
    symbols: [GinDataLeafPageGetItems, GinDataPageAddPostingItem]
---

# GIN 数据页

## 一句话职责

GIN 数据页负责以有序 TID posting list 保存词项关联的堆位置，并支持范围定位、重压缩和页分裂。

## 机制是什么

GIN 将词项到堆元组 TID 的关联存放在 posting list；数据树叶页承载 posting tree 的叶端列表，内部页以 `PostingItem` 的键界和子块号导航。叶页可为压缩格式，读取时统一解码成有序 TID 数组；内部页则通过插入、删除 `PostingItem` 维护分支。

本机制只管理 posting tree 数据页上的 TID 序列及结构，不负责词项键到 posting tree 根的 entry tree 查找，也不替代堆可见性检查。调用者负责定位目标数据页并持有恰当 buffer 锁；本文件预备页映像后在 critical section 应用变更，持久化恢复由 GIN WAL redo 层负责。

## 怎么实现的

1. `GinDataLeafPageGetItems` 检查页是否压缩：压缩页根据首个 posting segment 及长度，按需跳过整个首值不大于 `advancePast` 的段，再解码剩余段；旧式未压缩页则从数据区和 opaque 的 `maxoff` 取得 TID，并复制到调用者数组。提示只跳过整段，返回结果仍可能包含小于界值的 TID，调用者需要再筛选。
2. 插入准备阶段把页拆解为 `disassembledLeaf` 的双向链段；每个 `leafSegmentInfo` 保存段内容、TID 数量及 `UNMODIFIED/INSERT/REPLACE/ADDITEMS/DELETE` 动作。新 TID 与对应旧段合并并去重，空页则直接形成新段。
3. 重打包按 `GinPostingListSegmentMaxSize`、target、min 三个源码常量控制段最大、目标和合并下限；超长段再切分，过小段尝试与后段合并。容量不够则计算左右分段边界，非建索引插入倾向均衡；尾部追加则倾向为后续追加预留空间。
4. 若能留在一页，先在临时结构中完成重编码和 WAL 数据准备，再进入 critical section 改写页、置脏并登记 WAL buffer。`GinDataPageAddPostingItem` 在内部页按 offset 移动后续条目、写入新的 `PostingItem` 并更新 `maxoff` 与数据区下界。
5. 若需分裂，叶页先构造左右临时页映像和右界；内部页则把旧条目、新条目及更新后的 downlink 合并后分配到两页。上层 GIN 树维护流程负责分配新页及递归插入 downlink。

## 关键取舍

- 源码事实：压缩叶页拆成多个 posting segment，常量把 segment 控制在有限字节区间；较小段便于随机访问，代价是段头开销和更多边界管理。单个超大 posting list 虽省结构开销，却会让随机查找解码工作扩大。
- 源码事实：尾部追加在无法一页容纳时可把左页目标控制在约四分之三容量；普通插入则趋向两页平衡，build 过程可尽量填满左页。架构推论：这在顺序追加预期与一般后续负载之间折中，避免每次追加都立即产生低利用率新右页。
- 源码事实：预备阶段不修改原 buffer，真实提交在 critical section 内进行；这避免页分裂计算或分配失败时把部分构造状态暴露为已修改页面。代价是需要临时页映像及额外内存拷贝。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/gin/gindatapage.c` | `GinDataLeafPageGetItems` | 解码 posting leaf 的 TID，支持跳过完整 segment 的定位提示 |
| `src/backend/access/gin/gindatapage.c` | `GinDataPageAddPostingItem` | 在内部页插入子树指针并同步 opaque 条目数及标准页边界 |

## 参考

- [PostgreSQL 文档：GIN 索引](https://www.postgresql.org/docs/current/gin.html) —— 说明 GIN 倒排索引和 posting list 的总体用途。
- [PostgreSQL 官方源码：GIN 数据页](https://github.com/postgres/postgres/blob/master/src/backend/access/gin/gindatapage.c) —— 支撑叶页 posting segment 读写与拆分实现。

## 与主核 / AI 的关系

暂无直接对应。主核可借鉴“排序列表分段、以页边界作导航”的结构思路，但其 TID 语义、WAL 与并发协议不能由此页直接替代。
