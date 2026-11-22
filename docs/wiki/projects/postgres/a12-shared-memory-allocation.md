---
project: postgres
feature: shared-memory-allocation
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/ipc/shmem.c
    symbols: [ShmemInitStruct]
  - path: src/backend/storage/ipc/shmem_hash.c
    symbols: [ShmemInitHash]
---

# 主共享内存分配

## 一句话职责

主共享内存机制在启动期规划并建立一个进程间共享的固定区域，再以命名分配让各后端访问共同状态。

## 机制是什么

Postmaster 建立主共享内存段，后端继承或重新附接该映射；该机制提供固定大小命名对象分配以及建立于其上的共享哈希表。传统主共享内存映射在所有进程中位于相同地址，因此其中可使用普通指针。它不同于 DSM：DSM 段可在服务生命周期中动态创建，映射地址不保证一致，跨进程引用需用偏移或 DSA 指针。

该层不提供对象级自动回收：主共享内存分配后不能释放。哈希表删除条目可复用其自身空闲链表中的槽位，但不等同于释放整块共享区域。新增扩展的主共享内存需求应在启动请求阶段申报；启动后可用空间有限且不保证足够。

## 怎么实现的

新式接口以 `ShmemRequestStruct`/`ShmemRequestHash` 及注册回调分阶段工作：request 回调汇总所有子系统所需容量，随后 postmaster 分配总段，再由 init 回调初始化对象；EXEC_BACKEND 子进程还需重建请求信息并 attach。`shmem.c` 的主初始化为分配器和 `ShmemIndex` 建立基础设施，索引记录名称、大小与区域位置。

`ShmemInitStruct` 是保留的旧式接口：调用者须在启动请求阶段或相应的启动初始化路径中确保容量已预留，再于 startup hook 中查找或建立命名条目，并依据 `found` 判定是否应初始化内容；它在 `ShmemIndexLock` 独占锁下查找/插入。它与上面的新式 request/init 接口不同，不会自行向集中请求回调登记容量。`ShmemInitHash` 同样是兼容接口，先以估算大小调用 `ShmemInitStruct`，再创建或附接哈希表。共享哈希的目录和元素在创建时一次性预分配，固定容量不能增长，因为底层共享区域不能随表扩展。两者都假设调用方已保证不会并发重复初始化。

## 关键取舍

- **启动期集中规划**：换成运行期任意扩容会让各进程的映射/指针与容量协调复杂化；主段采用统一固定容量。
- **固定地址与普通指针**：简化进程间结构访问；换成可变映射地址则需偏移引用，复杂度更高但更适合动态段。
- **命名索引而非可释放堆**：对象生命周期与服务器主段绑定；换成一般堆分配器可回收更灵活，但需额外处理跨进程并发与碎片、指针稳定性。
- **共享哈希固定大小**：预估容量后一次分配，避免底层扩区；代价是无法按需扩表。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/ipc/shmem.c` | `ShmemInitStruct` | 命名结构的兼容查找/建立接口 |
| `src/backend/storage/ipc/shmem.c` | `ShmemAlloc` | 从主段分配对齐内存 |
| `src/backend/storage/ipc/shmem_hash.c` | `ShmemInitHash` | 创建或附接兼容共享哈希表 |
| `src/backend/storage/ipc/shmem_hash.c` | `shmem_hash_create` | 配置固定容量共享哈希分配器 |

## 参考

- [PostgreSQL 18 官方文档：共享内存与恢复参数](https://www.postgresql.org/docs/18/runtime-config-resource.html)
- [PostgreSQL 官方文档：扩展共享内存初始化](https://www.postgresql.org/docs/18/xfunc-c.html#XFUNC-SQL-FUNCTIONS-RETURNING-SET)
- [PostgreSQL 源码：shmem.c](https://github.com/postgres/postgres/blob/master/src/backend/storage/ipc/shmem.c)

## 与主核 / AI 的关系

主共享内存是跨 PostgreSQL 后端的共享状态底座，不等同于应用层缓存服务；AI 可把 request/init/attach 看作容量规划与映射生命周期阶段，但不可据此假设对象支持任意释放或增长。
