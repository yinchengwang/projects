#ifndef DB_SPI_REGISTRY_H
#define DB_SPI_REGISTRY_H

#include "common/error.h"
#include "common/types.h"
#include "spi/storage_am.h"

/*
 * 虚拟表注册表 —— 模态向 SQL 提供表的入口（v0.9）。
 *
 * 【问题】
 *
 * 模态不能碰 catalog（硬规则 6：模态各自管自己的元数据）。
 * 那模态怎么让 `SELECT ... FROM blob_frames` 这条 SQL 能跑？
 *
 * 【答案】
 *
 * 注册表在**实例级**（所有连接共享），存的是"名字 → StorageAM*"。
 *
 * 它是纯运行时结构，**不落盘**：
 *   - blob 的两张表每次启动都重新注册（blob 自己的目录扫一遍就出来了）
 *   - 关系模态的表不走注册表 —— 它们在 catalog 里，是持久化的元数据
 *
 * 【解析顺序】
 *
 *   1. catalog（用户建的表）—— 用户显式建的东西应该赢
 *   2. 注册表（模态提供的虚拟表）
 *   3. 都没有 → "表不存在"
 *
 * 【注册表不知道 blob 是什么】
 *
 * 它只知道"名字 → 一个 StorageAM"。所以第 9 个模态接进来，
 * 只是多注册两条记录，执行器一行不改。
 */

typedef struct SpiRegistry SpiRegistry;

int  spi_registry_create(SpiRegistry **out);
void spi_registry_destroy(SpiRegistry *r);

/*
 * 注册一张虚拟表。
 *
 * am 归注册表**借用**（不拷贝、不拥有）——
 * 调用方要保证 am 活到注册表销毁之后（实例级持有即可）。
 *
 * 重名返回 E_INVAL。
 */
int spi_registry_add(SpiRegistry *r, const char *name, StorageAM *am);

/* 按名字找。找不到返回 NULL。 */
StorageAM *spi_registry_find(const SpiRegistry *r, const char *name);

/* 注册了几张表（诊断用） */
u32 spi_registry_count(const SpiRegistry *r);

#endif /* DB_SPI_REGISTRY_H */
