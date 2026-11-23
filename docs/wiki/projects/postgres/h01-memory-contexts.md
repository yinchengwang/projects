---
project: postgres
feature: memory-contexts
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/mmgr/mcxt.c
    symbols: [MemoryContextInit, MemoryContextAlloc, MemoryContextReset, palloc]
  - path: src/backend/utils/mmgr/aset.c
    symbols: [AllocSetContextCreateInternal, AllocSetAlloc]
---

# 内存上下文体系

## 一句话职责

内存上下文把分配的生存期组织成可切换、可批量 reset/delete 的层级，并让 `palloc` 使用当前上下文完成请求。

## 机制是什么

`MemoryContext` 是分配器无关的接口对象，保存父子关系、实现方法表、名称、重置回调和记账状态；具体 allocator 通过方法表实现 alloc/reset/delete。`CurrentMemoryContext` 让调用代码可在局部切换分配归属，`palloc` 快速调用当前 context 的 `alloc` 方法。`AllocSet` 是标准分配实现，以多个 block 统筹较小 chunk，并对大请求采用独立 block 路径。

上下文负责内存块的所有权及批量生命周期，不负责 libc 对象或文件、buffer pin、锁等非内存资源的释放；这些须由其他机制管理。`MemoryContextReset` 保留命名 context 本身并重置其内容、删除后代 context；`MemoryContextDelete` 则连 context 节点与后代一并删除。reset 释放的是该 allocator 的可回收空间语义，并不保证每次都把保留 block 归还操作系统。

## 怎么实现的

1. `MemoryContextInit` 创建 `TopMemoryContext` 并设为初始 `CurrentMemoryContext`，再创建供错误报告使用的 `ErrorContext`；后续 context 通过 `MemoryContextCreate` 初始化通用头并链接到父节点的 child list。
2. `AllocSetContextCreateInternal` 校验 block 参数，优先复用相同尺寸类别的 reset context freelist；否则通过 malloc 建立 keeper block，再初始化 AllocSet 私有状态与方法表。
3. `palloc` 读取 `CurrentMemoryContext`，校验 context 与 critical section 限制，将 `isReset` 清除后调 `context->methods->alloc`。显式 `MemoryContextAlloc` 则以传入 context 做相同分派。
4. `AllocSetAlloc` 对超过 chunk 限制的大请求走独立 block；常规请求按 size class 查 freelist，或从当前 block bump 分配。空间不足时申请新 block，并更新 chunk/header 与记账信息。
5. `MemoryContextReset` 先删除所有后代，再调用 `MemoryContextResetOnly`；后者先按注册顺序逆序运行 reset callbacks，再调用 allocator 的 reset。AllocSetReset 清空 free lists、释放非 keeper blocks、重用 keeper block，降低重复短期分配的 malloc churn。
6. `MemoryContextDelete` 迭代式从叶节点向根删除，以免递归清理在错误处理期间触发栈深问题；每个 context 执行回调、allocator 删除并从父子树摘除。内存分配失败进入统一 OOM 错误路径，若设置 NO_OOM 则 allocator 可返回 NULL。

## 关键取舍

- **源码事实：**AllocSet 将小请求分入按大小类别复用的 chunk/free list，block 空间采用 bump 分配；较大请求走独立 block，reset 时保留 keeper block。**架构推论：**以小块内部碎片与保留少量内存为代价，避免每次分配都向系统 allocator 请求内存；完全逐次 malloc/free 更能即时归还内存，但会增加调用开销及碎片风险。
- **源码事实：**context tree 使 reset 可删除所有后代 context，而 reset 本身保留 context 节点；delete 迭代式自叶向上执行。**架构推论：**调用方可复用同一工作区管理对象并通过树界定依赖生命周期；如果每个对象独立释放，就要显式维护大量释放点，错误路径容易漏清理。
- **源码事实：**`palloc` 直调当前 context 方法以降低通用包装开销，且默认不允许 critical section 中可能失败的分配；ErrorContext 被特别允许。**架构推论：**保留快速路径与错误报告的最低可用内存，代价是调用者必须提前安排临界区所需空间，不能把一般分配当作无风险操作。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/mmgr/mcxt.c` | `MemoryContextInit` | 建立顶层与错误处理 context，并设置初始当前 context。 |
| `src/backend/utils/mmgr/mcxt.c` | `MemoryContextAlloc` | 按指定 context 的 allocator 方法分配内存。 |
| `src/backend/utils/mmgr/mcxt.c` | `MemoryContextReset` | 删除后代并重置当前 context 的分配内容。 |
| `src/backend/utils/mmgr/mcxt.c` | `palloc` | 走 `CurrentMemoryContext` 的轻量分配入口。 |
| `src/backend/utils/mmgr/aset.c` | `AllocSetContextCreateInternal` | 创建或复用标准 AllocSet context 与初始 block。 |
| `src/backend/utils/mmgr/aset.c` | `AllocSetAlloc` | 以 freelist、当前 block 或独立大块满足请求。 |

## 参考

- [PostgreSQL 文档：内存上下文统计](https://www.postgresql.org/docs/current/monitoring-stats.html) —— 介绍观察内存上下文统计信息的接口。
- [PostgreSQL 官方源码：MemoryContext 核心](https://github.com/postgres/postgres/blob/master/src/backend/utils/mmgr/mcxt.c) —— 对照 context 树、reset/delete 和 palloc 分派。

## 与主核 / AI 的关系

暂无直接对应。可借鉴主核短生命周期对象按上下文批量回收的边界；allocator 具体策略及错误期语义应由主核自身内存模型决定。
