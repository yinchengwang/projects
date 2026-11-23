---
project: postgres
feature: resource-owners
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/resowner/resowner.c
    symbols: [ResourceOwnerCreate, ResourceOwnerRemember, ResourceOwnerRelease, ResourceOwnerDelete]
---

# 资源所有者与错误期回收

## 一句话职责

ResourceOwner 为查询与事务范围内的非内存资源登记所有权，并在完成或中止时按依赖顺序批量释放。

## 机制是什么

ResourceOwner 是可嵌套的资源归属节点；每种资源由 `ResourceOwnerDesc` 定义释放阶段、优先级与释放回调。资源 owner 记录 buffer pin、引用类资源、临时文件等类型，并以局部数组、hash table 和独立锁缓存兼顾近期频繁 remember/forget 与大量长持有资源。

它不替代资源本身的常规释放 API，也不负责分配这些对象的内存；正常路径可以及时 forget/release，owner 主要保证异常退出时仍可回收。释放被拆成锁前、锁阶段、锁后多个 phase，以便先清理对其他 backend 可见的资源，再释放锁，最后清理 backend 内部资源。

## 怎么实现的

1. `ResourceOwnerCreate` 在 `TopMemoryContext` 分配并清零 owner，设置 parent-child 链与调试名称。`CurrentResourceOwner` 等全局指针指示当前操作归属层级。
2. 资源获取方先调用 `ResourceOwnerEnlarge` 预留记录空间，再获取资源并用 `ResourceOwnerRemember` 记录 `Datum` 与 `ResourceOwnerDesc`；预扩容先于资源获取，避免资源已取得后记录扩容 OOM 导致泄漏。
3. 少量近期对象进入固定数组，数组满时移入开放寻址 hash；锁引用有专用小缓存，超出后由 lock manager 的本地表路径处理。`ResourceOwnerForget` 在正常释放后移除一项记录。
4. `ResourceOwnerRelease` 对当前 owner 及后代逐 phase 递归执行；首次释放时按 `ResourceOwnerDesc.release_phase` 和优先级排序，并设置 releasing 状态，阻止回调期间新增/forget 记录破坏遍历。
5. BEFORE_LOCKS 阶段先释放 buffer pins 等资源，避免释放锁后其他 backend 观察到事务仍持有可见资源；LOCKS 阶段依顶层/子事务及 commit/abort 语义释放或转移锁；AFTER_LOCKS 阶段处理剩余内部资源。每个资源从表中移除后才调用释放回调，避免回调报错后重复释放。
6. 完成所有资源释放后，调用者才能 `ResourceOwnerDelete`；它要求资源已清空，递归删除子 owner、从父树摘除再释放 owner 内存。错误中止路径会运行释放 phase，避免错误退出留下 pins、文件或其他已登记资源。

## 关键取舍

- **源码事实：**近期资源先存固定数组，满后进入 hash；释放前将资源按 phase/priority 排序，子 owner 先于父 owner 递归处理。**架构推论：**以释放时排序开销换取常见 remember/forget 的低成本与一致释放顺序；若每次操作都维护全局有序结构，快路径需要更多搬移/平衡开销。
- **源码事实：**调用者必须先 `ResourceOwnerEnlarge` 再取资源，再 `ResourceOwnerRemember`，错误时可释放已登记资源。**架构推论：**把可能失败的空间预留放在获得外部资源之前，降低错误处理时“资源已到手、登记失败”的泄漏窗口；若先获取后扩容，需要额外逐调用点回滚逻辑。
- **源码事实：**release phase 分为锁前、锁、锁后三段，锁前阶段释放对其他 backend 可见的 pins 等资源。**架构推论：**避免另一 backend 在锁被释放后观察到旧事务仍持有 buffer pin 等状态；若不分阶段而随意释放，可能暴露短暂不一致的并发状态。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/resowner/resowner.c` | `ResourceOwnerCreate` | 在顶层内存上下文创建 owner 并接入父子树。 |
| `src/backend/utils/resowner/resowner.c` | `ResourceOwnerRemember` | 将资源标识及资源类型描述登记到 owner。 |
| `src/backend/utils/resowner/resowner.c` | `ResourceOwnerRelease` | 按 release phase 递归释放 owner 与后代资源。 |
| `src/backend/utils/resowner/resowner.c` | `ResourceOwnerDelete` | 确认资源清空后删除 owner 节点及其子节点。 |

## 参考

- [PostgreSQL 源码文档：资源所有者说明](https://github.com/postgres/postgres/blob/master/src/backend/utils/resowner/README) —— 介绍 ResourceOwner 的使用约定。
- [PostgreSQL 官方源码：ResourceOwner 实现](https://github.com/postgres/postgres/blob/master/src/backend/utils/resowner/resowner.c) —— 对照登记结构、释放顺序与异常回收路径。

## 与主核 / AI 的关系

暂无直接对应。主核可借鉴“资源类别声明释放阶段/优先级、事务错误路径统一兜底”的设计；不能以 ResourceOwner 代替内存上下文或资源自身的正常关闭协议。
