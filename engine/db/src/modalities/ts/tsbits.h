#ifndef DB_MODALITIES_TS_TSBITS_H
#define DB_MODALITIES_TS_TSBITS_H

#include "common/error.h"
#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * tsbits.h —— 时序模态的**位流基元**（v1.5 步骤 1）
 *
 * 【为什么先有这个文件】
 *
 * 三件套（delta-of-delta / RLE / Gorilla）都要在**位**的粒度上读写：
 * "常间隔 → 0x0 一个 bit"、"XOR 为 0 → 一个 bit"，
 * 这些编码的最小单位是 bit，不是 byte。
 * 三个编码各写一套位流 ⇒ 三份几乎一样的代码 ⇒ 迟早有一份的位序不一样
 * ⇒ 读出来全是垃圾，而且**不报错**。
 *
 * 所以位流只有**一份**，三个编码都用它。
 * （同一个道理：坑 M5-12 "语义只能有一个主人"。）
 *
 * 【位序：LSB-first】
 *
 * 一个字节里，第 0 个写的 bit 落在**最低位**。
 * 理由： Gorilla 论文的伪代码是这么写的，对照论文最省事；
 * 而且"先写控制位再写数据位"时，控制位落在低位，
 * 读的时候先取低位 ⇒ 解析时不必先跳字节。
 *
 * 【写不下的处理】
 *
 * 写超出容量 ⇒ 置 `overflow` 并返回 E_NOSPACE；
 * **已经写进去的位不算数**（调用方看到 E_NOSPACE 就整块作废）。
 *
 * 【读越界的处理】
 *
 * 读超出长度 ⇒ 置 `bad` 并返回 E_CORRUPT。
 * 注意：**解码端的错误必须是硬错误**，不能"读不出来就填 0" ——
 * 那样会把一段坏数据静默变成一段看着正常的数据。
 */

typedef struct {
    u8  *buf;        /* 目标缓冲（调用方给） */
    u32  cap;        /* 容量（字节） */
    u32  len;        /* 已用字节数 */
    u32  nbits;      /* 已用位数（= len * 8 - 字节内剩余位） */
    bool overflow;   /* 写过界 */
} TsBitWriter;

/* 初始化写入器（buf / cap 由调用方给，buf 可以为 NULL 当 cap=0 的探针） */
void tsbw_init(TsBitWriter *w, u8 *buf, u32 cap);

/* 写 nbits 个位（nbits ∈ [0, 64]；value 的高位被忽略） */
int tsbw_write_bits(TsBitWriter *w, u64 value, u32 nbits);

/* 写 1 个位 */
int tsbw_write_bit(TsBitWriter *w, bool bit);

/* 写"变长整数"（每组 8 位：低 7 位是数据、最高位是续位；**每组都写满 8 位**） */
int tsbw_write_varint(TsBitWriter *w, u64 value);

/* 已用位数 */
u32 tsbw_bit_length(const TsBitWriter *w);

/* 已用字节数（向上取整：最后那个字节里的零位也算占位） */
u32 tsbw_byte_length(const TsBitWriter *w);

/* 收尾：把最后一个字节补齐（高位补 0）。返回总字节数，失败 -1 */
int tsbw_finish(TsBitWriter *w, u32 *bytes_out);

typedef struct {
    const u8 *buf;
    u32  len;      /* 字节数 */
    u32  nbits;    /* 总位数 = len * 8 */
    u32  pos;      /* 当前位号（从 0 开始） */
    bool bad;      /* 越界 / 格式错 */
} TsBitReader;

void tsbr_init(TsBitReader *r, const u8 *buf, u32 len);

/* 读 nbits 个位（nbits ∈ [0, 64]） */
int tsbr_read_bits(TsBitReader *r, u32 nbits, u64 *out);

/* 读 1 个位 */
int tsbr_read_bit(TsBitReader *r, bool *out);

/* 读变长整数 */
int tsbr_read_varint(TsBitReader *r, u64 *out);

/* 读完了没有（位级） */
bool tsbr_eof(const TsBitReader *r);

/* ---------- zigzag：把有符号数映射成无符号，便于变长编码 ---------- */

/* i64 → u64（偶数映到偶数，奇数映到奇数 ⇒ 保序且可逆） */
static inline u64 ts_zigzag_i64(i64 v)
{
    return ((u64)v << 1) ^ (u64)(v >> 63);
}

/* u64 → i64 */
static inline i64 ts_unzigzag_i64(u64 z)
{
    return (i64)((z >> 1) ^ (~(z & 1u) + 1u));
}

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_TS_TSBITS_H */
