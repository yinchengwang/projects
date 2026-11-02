#ifndef DB_STORAGE_RECORD_H
#define DB_STORAGE_RECORD_H

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"

/*
 * tuple 编解码 —— 一行数据（多列）与字节串之间的转换。
 *
 * 布局（DESIGN.md §3.1）：
 *
 *   ┌──────────┬──────────┬─────────┬──────────────┬────────────┬──────────────┬──────────┐
 *   │ xmin 8B  │ xmax 8B  │ flags4B │ null bitmap  │  定长列区  │ 变长偏移数组 │ 变长数据 │
 *   └──────────┴──────────┴─────────┴──────────────┴────────────┴──────────────┴──────────┘
 *                                       ceil(n/8)                  2B × n_varlen
 *
 * 字节序约定（容易翻车，记牢）：
 *   - 这里的所有数值都是 **小端**（页内格式，x86 友好）。
 *   - 索引 key 是 **大端**（v0.3 在 btree 里做），因为 memcmp 要等价数值比较。
 *   同一个系统里两种字节序，混用会导致"索引查不到数据"。
 *
 * 解码必须依赖 schema —— tuple 自身不自描述。schema 从 catalog 拿。
 */

#define REC_HDR_SIZE   20u   /* xmin 8 + xmax 8 + flags 4 */
#define R_XMIN         0u
#define R_XMAX         8u
#define R_FLAGS        16u

/* flags 位 */
#define REC_F_COMMITTED 0x01u
#define REC_F_ABORTED   0x02u
#define REC_F_OLDVER    0x04u

typedef enum {
    T_INT4 = 1,
    T_INT8 = 2,
    T_BOOL = 3,
    T_TEXT = 4,
    T_REAL = 5    /* v1.1：浮点。AVG / 地理坐标 / 时序数值 / JSON 数字 */
} ColType;

#define COL_NAME_MAX 32

/*
 * 列定义。name 用定长数组而不是 const char*，
 * 这样 catalog 可以按值拷贝 schema，不用管字符串生命周期。
 */
typedef struct {
    char name[COL_NAME_MAX];
    u8   type;        /* ColType */
    bool nullable;
} ColDesc;

typedef struct {
    bool is_null;
    union {
        i32  i4;
        i64  i8;
        bool b;
        double f8;                            /* T_REAL */
        struct { const u8 *p; u16 n; } text;   /* 解码时指向 buf 内部 */
    } u;
} Value;

/* 一行最多多少列（rec_decode 的上限） */
#define REC_MAX_COLS 32

/*
 * ==================== 记录布局（预计算） ====================
 *
 * 布局只跟**列定义**有关，跟行无关：
 *   null bitmap 多宽、定长区从哪开始、每列在定长区的偏移、
 *   变长列是第几个变长列……
 *
 * 【为什么要把它显式算出来】
 * 原来的 rec_decode 每行都重算一遍：
 *   - layout_of()  一次 O(ncols) 的循环
 *   - fixed_off_of() 每列一次 O(ncols) 的循环  → 整行 O(ncols²)
 *   - var_idx_of()  每个变长列一次 O(ncols) 的循环
 *
 * 10 万行就是 10 万次无谓的重复计算。
 * v1.1 的执行器基准实测：**解码阶段占了整条查询的 49%**，
 * 而这里面九成以上是在重算同一份布局。
 *
 * 布局是**表的属性** —— 算一次，用一辈子。
 * AM 在建表上下文时算好（heap_am_build），解码时直接用。
 */
typedef struct {
    u16 bitmap;      /* null bitmap 字节数 */
    u16 fixed_off;   /* 定长区起点 */
    u16 fixed_len;   /* 定长区总宽 */
    u16 varoff_off;  /* 变长偏移数组起点 */
    u16 nvar;        /* 变长列个数 */
    u16 data_off;    /* 变长数据起点 = 固定开销 */
    u16 ncols;       /* 列数 */

    /* 每列的位置：解码时 O(1) 取出，不用再循环推导 */
    u16 fixed_col_off[REC_MAX_COLS];   /* 定长列偏移；变长列填 0xFFFF */
    u16 var_col_idx[REC_MAX_COLS];     /* 变长列序号；定长列填 0xFFFF */
} RecLayout;

/* 算一次布局。cols 不变的话，结果一直有效。 */
int rec_layout(const ColDesc *cols, int ncols, RecLayout *out);

/*
 * 不含变长数据的固定开销：header + null bitmap + 定长列区 + 变长偏移数组。
 * 调用方用它判断缓冲区够不够。
 */
int rec_fixed_size(const ColDesc *cols, int ncols, u16 *out);

/*
 * 编码。buf 由调用方提供，容量 bufsize。
 * 非 nullable 列传 is_null 返回 E_INVAL。
 * 成功时 *out_len 为实际占用字节数（含变长数据）。
 */
int rec_encode(const ColDesc *cols, int ncols, const Value *vals,
               u8 *buf, u16 bufsize, u16 *out_len);

/*
 * 解码。len 是 tuple 总长（page_get 给出的那个）。
 * 变长列的 p 指向 buf 内部，buf 失效则 p 失效。
 *
 * 这是"每行重算布局"的版本 —— 热路径请用 rec_decode_layout。
 */
int rec_decode(const ColDesc *cols, int ncols, const u8 *buf, u16 len, Value *vals);

/* 用预计算好的布局解码 —— 热路径用这个 */
int rec_decode_layout(const RecLayout *L, const ColDesc *cols,
                      const u8 *buf, u16 len, Value *vals);

/* 只解一列（投影下推时用，避免整行解码）。 */
int rec_get_col(const ColDesc *cols, int ncols, const u8 *buf, u16 len,
                int col, Value *out);

/* 头部字段 */
u64  rec_xmin(const u8 *buf);
void rec_set_xmin(u8 *buf, u64 xmin);
u64  rec_xmax(const u8 *buf);
void rec_set_xmax(u8 *buf, u64 xmax);
u32  rec_flags(const u8 *buf);
void rec_set_flags(u8 *buf, u32 flags);

/* 类型宽度（变长返回 0） */
u16 rec_type_width(u8 type);

#endif /* DB_STORAGE_RECORD_H */
