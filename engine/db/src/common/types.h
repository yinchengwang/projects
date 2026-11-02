#ifndef DB_COMMON_TYPES_H
#define DB_COMMON_TYPES_H

/*
 * 定宽类型 + 小端序列化。
 *
 * 铁律：
 *   1. 磁盘格式只用 u8/u16/u32/u64，绝不用 int/long/size_t
 *      （long 在 Windows 是 4 字节、Linux 是 8 字节，文件跨平台就打不开）
 *   2. 绝不 memcpy(struct) 到磁盘：填充字节是未初始化内容，既脏又泄漏
 *   3. 字节序统一小端，读写都走下面的函数
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

/* ---------- 小端写入 ---------- */
static inline void put_u16(u8 *p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static inline void put_u32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static inline void put_u64(u8 *p, u64 v) {
    put_u32(p,     (u32)v);
    put_u32(p + 4, (u32)(v >> 32));
}

/* ---------- 小端读取 ---------- */
static inline u16 get_u16(const u8 *p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static inline u32 get_u32(const u8 *p) {
    return (u32)p[0]
         | ((u32)p[1] << 8)
         | ((u32)p[2] << 16)
         | ((u32)p[3] << 24);
}

static inline u64 get_u64(const u8 *p) {
    return (u64)get_u32(p) | ((u64)get_u32(p + 4) << 32);
}

/* 页号约定：0 保留给元数据页，UINT32_MAX 表示"无" */
#define PAGE_ID_NIL   ((u32)0xFFFFFFFFu)
#define PAGE_ID_META  ((u32)0)

/*
 * RID —— 行在堆表里的物理地址，索引里存的就是它。
 *
 * 之所以用 (page, slot) 而不是行指针：页会被逐出、文件会被重开，
 * 只有"页号 + 槽位"是稳定的。
 */
typedef struct {
    u32 page;
    u16 slot;
} Rid;

static inline Rid rid_make(u32 page, u16 slot)
{
    Rid r;
    r.page = page;
    r.slot = slot;
    return r;
}

static inline bool rid_is_nil(Rid r)
{
    return r.page == PAGE_ID_NIL;
}

/* 打包成 u64，便于在索引项里当定长 key 的一部分 */
static inline u64 rid_to_u64(Rid r)
{
    return ((u64)r.page << 16) | (u64)r.slot;
}

static inline Rid rid_from_u64(u64 v)
{
    return rid_make((u32)(v >> 16), (u16)(v & 0xFFFFu));
}

#endif /* DB_COMMON_TYPES_H */
