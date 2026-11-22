---
project: postgres
feature: dynamic-shared-memory
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/ipc/dsm.c
    symbols: [dsm_create, dsm_attach, dsm_detach]
  - path: src/backend/storage/ipc/dsm_impl.c
    symbols: [dsm_impl_op, dsm_impl_pin_segment, dsm_impl_unpin_segment]
---

# 动态共享内存 DSM

## 一句话职责

DSM 在服务运行期间创建、附接和销毁可动态管理的共享内存段，并跟踪其映射生命周期。

## 机制是什么

`dsm.c` 在平台实现之上提供带控制段、引用计数、资源所有者清理与 detach 回调的高层设施；`dsm_impl.c` 则把 create/attach/detach/destroy 委派给 POSIX、System V、Windows 或 mmap 后端。配置了预留主段空间时，新段也可能从主共享内存中切片取得，否则由平台机制创建独立段。与主共享内存不同，DSM 映射地址不保证跨进程相同，结构内应使用偏移或 DSA 指针。

DSM 不是命名共享对象的主段分配器，也不自动定义段内对象格式、同步策略或消息协议；这些由使用者（例如 shm_mq）负责。`dsm_handle` 用于其他进程请求附接，不是可直接跨进程解引用的地址。

## 怎么实现的

`dsm_create` 建立后端私有描述符，必要时从预留区的 FreePageManager 分配页，否则以随机句柄循环尝试平台 create。控制段在 `DynamicSharedMemoryControlLock` 下登记 handle、引用计数及主段分配位置；计数从 2 起步，源码注释规定到 1 时触发销毁，因此 2 或以上表示活动状态。这是控制表为区分仍可附接与已进入消亡阶段所用的内部基准，不应把它解读成“恰有两个实际进程引用”；创建时加一的基准并不都代表当前映射参与者。附接方匹配活动条目后再增加引用。

`dsm_attach` 拒绝同一后端对同一 handle 重复附接；查控制段时跳过已进入消亡状态的条目，找到后增加引用，再映射非主段后端对象。句柄已被所有其他参与者释放时可返回 NULL。`dsm_detach` 先运行使用者注册的 detach 回调并解除映射，再减少引用；到达 1 时销毁段或归还预留区页面并释放控制槽。资源所有者负责查询/错误清理，`dsm_pin_mapping` 可延长映射到会话生命周期；硬崩溃遗留段会在 postmaster 启动清理时处理。平台低层 API 的 pin/unpin 是另一层面：主要用于保留无人附接段，Windows 需在 postmaster 中保留/关闭额外句柄。

## 关键取舍

- **句柄与偏移而非稳定绝对地址**：适配不同映射地址；换成要求同址映射能简化指针使用，但受地址空间布局及平台限制。
- **引用计数与延迟销毁**：允许多个进程独立附接；换成创建者独占生命周期会让工作进程无法安全延长使用期。
- **统一高层清理加多种平台后端**：调用者不直接管理 OS 原语；代价是高层控制表与底层平台状态都需协调。
- **主段预留区优先，不足再走平台创建**：降低创建独立共享对象的需要，但需锁与自由页管理；换成完全依赖外部系统段会牺牲资源管理的一致性。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/ipc/dsm.c` | `dsm_create` | 分配主段预留页或创建独立段并登记控制信息 |
| `src/backend/storage/ipc/dsm.c` | `dsm_attach` | 按 handle 增加引用并建立本进程映射 |
| `src/backend/storage/ipc/dsm.c` | `dsm_detach` | 回调、解除映射、减引用并在末引用时销毁 |
| `src/backend/storage/ipc/dsm_impl.c` | `dsm_impl_op` | 按配置选择平台 create/attach/detach/destroy 操作 |
| `src/backend/storage/ipc/dsm_impl.c` | `dsm_impl_pin_segment` / `dsm_impl_unpin_segment` | 平台相关的无人附接段保留处理 |

## 参考

- [PostgreSQL 官方文档：动态共享内存类型配置与预留容量](https://www.postgresql.org/docs/18/runtime-config-resource.html#GUC-DYNAMIC-SHARED-MEMORY-TYPE)
- [PostgreSQL 源码：DSM 高层生命周期与平台实现](https://github.com/postgres/postgres/blob/master/src/backend/storage/ipc/dsm.c)
- [PostgreSQL 源码：DSM 平台后端](https://github.com/postgres/postgres/blob/master/src/backend/storage/ipc/dsm_impl.c)

## 与主核 / AI 的关系

DSM 是并行工作进程等机制可复用的共享内存载体；AI 应区分逻辑生命周期管理（dsm.c）与 OS 共享段操作（dsm_impl.c），不应把 handle 当作地址或假设同址映射。
