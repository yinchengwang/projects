---
project: postgres
feature: gist-search-insert
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/gist/gist.c
    symbols: [gistinsert]
  - path: src/backend/access/gist/gistproc.c
    symbols: [rt_box_union, box_penalty]
---

# GiST 搜索与插入

## 一句话职责

GiST 通过 opclass 提供的键判定与扩张规则，把新索引项导向合适子树并在扫描时剪除不可能匹配的分支。

## 机制是什么

GiST 是可扩展的平衡树访问方法；内部 downlink 保存对其下方项的摘要键，具体键如何一致性判定、合并和计算插入代价由 opclass support procedure 定义。以几何 box 实现为例，父键是覆盖子项的最小包围盒，插入代价是并入新项后面积的增量。

本文聚焦插入路径与几何 support procedure 示例。它不负责定义所有 opclass 的业务语义，也不负责堆元组可见性或 SQL 谓词的最终判定；对于有损摘要，扫描端可能需要回表重检。页拆分及其 parent downlink 修复由页维护路径执行，非本页主要边界。

## 怎么实现的

1. `gistinsert` 通过 `IndexInfo.ii_AmCache` 缓存 `GISTSTATE`，把 SQL 值按列执行压缩函数形成 `IndexTuple`，将堆 TID 写入 tuple 后调用 `gistdoinsert`。
2. `gistdoinsert` 从根开始下降。页面先取共享 buffer lock 并记录 LSN；遇到需更新 downlink 或叶页插入时升级为排他锁，升级期间若 LSN 已变化则重试，避免按过期页面状态写入。
3. 内部页调用 `gistchoose`。它按索引列顺序比较 penalty，较早列优先；某项键需要扩张时由 `gistgetadjusted` 对旧摘要和待插入键调用 union，若 union 改变则回写父 downlink，然后继续向所选子页下降。
4. 几何 box 的 `rt_box_union` 逐坐标取边界极值；`box_penalty` 计算 union box 面积减原 box 面积。通用 GiST 核心不假定 box 格式，只调用已经加载到 `GISTSTATE` 的 support 函数。
5. 到达叶页后，以排他锁调用 tuple 插入路径；空间足够则写页，空间不足则分裂并沿 stack 修复父级 downlink。并发拆分可用 rightlink、`F_FOLLOW_RIGHT` 和 LSN/NSN 检查协调；出错留下的不完整拆分可由后续插入修补。
6. buffer pin 与锁随 stack 生命周期释放；用户定义 support procedure 在临时 memory context 调用，随后 reset，避免每次插入累积其分配。

## 关键取舍

- 源码事实：每一索引列 penalty 按定义顺序优先级逐列比较，而不是简单求和。这样允许 opclass 表达列优先次序；替代的加权和会使后列变化抵消更重要的前列代价，语义不再一致。
- 源码事实：相同 penalty 时使用伪随机选择是否保留当前候选。`gistchoose` 注释指出一直固定旧候选有利缓存局部性，但可能让相似键集中在分裂后的一侧、浪费另一页空间；完全均匀分流则牺牲局部性。
- 源码事实：页下降先共享锁，确定要更新时再换排他锁，并以 LSN 重验。相较从根到叶全程排他锁，这减少只读路径阻塞；代价是需要处理重试和并发拆分的 rightlink/NSN 交互。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/gist/gist.c` | `gistinsert` | 构建索引 tuple 并把插入交给树下降工作例程 |
| `src/backend/access/gist/gistproc.c` | `rt_box_union` | 生成两 box 的包围 union，作为几何摘要扩张基础 |
| `src/backend/access/gist/gistproc.c` | `box_penalty` | 计算 box 扩张面积代价，供几何 GiST 的选择逻辑使用 |

## 参考

- [PostgreSQL 文档：GiST 索引](https://www.postgresql.org/docs/current/gist.html) —— 说明 GiST 的可扩展索引框架与 opclass 支持函数。
- [PostgreSQL 官方源码：GiST 插入](https://github.com/postgres/postgres/blob/master/src/backend/access/gist/gist.c) —— 支撑插入下降、锁升级及父键更新流程。

## 与主核 / AI 的关系

暂无直接对应。GiST 的“摘要键、代价引导、保守剪枝”可作为主核索引设计参照，但其 opclass 契约和并发树维护并非通用查询执行器。
