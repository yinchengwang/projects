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
    T_REAL = 5,   /* v1.1：浮点。AVG / 地理坐标 / 时序数值 / JSON 数字 */
    /*
     * v1.6：JSON 文档（JSONB 二进制）。
     *
     * 与 T_TEXT 一样是**变长**类型 —— 值区放的是 **JSONB 字节**，
     * 不是 JSON 文本。这样查路径时不必重新解析。
     *
     * ⚠️ **T_JSON 与 T_TEXT 共用变长区**，但**类型不同** ——
     * 解码时靠 cols[i].type 区分，不靠字节内容猜。
     * （"靠内容猜类型"是万恶之源：JSONB 的第一个字节恰好是 tag，
     *  但文本的第一个字节也可能碰巧是 6。）
     */
    T_JSON = 6
} ColType;

/*
 * 这个类型是不是变长类型（值区放字节流）。
 *
 * ⚠️ **编码与解码两侧都必须用这个函数** ——
 * 两边各写一遍"哪些类型是变长的"，早晚会不一致（v1.5 坑 M6-63 同款纪律）。
 */
static inline bool col_is_var(u8 type)
{
    return type == T_TEXT || type == T_JSON;
}

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
 * ==================== JSON 溢出标记（C5.3b-b） ====================
 *
 * JSONB 超过 JSON_INLINE_MAX（2048）时不在 record 变长区里存字节，
 * 而是存一个**定长 12 字节的溢出标记**，指向一条 PT_JSON_OVF 页链。
 *
 * 布局（紧跟在变长区那一列的字节流开头）：
 *
 *   偏移 0   4B  tag = REC_JSON_OVF_TAG（0x4A534F56，"JSOV"）
 *   偏移 4   4B  chain_head：页链首页页号（u32；PAGE_ID_NIL 非法）
 *   偏移 8   4B  chain_len ：链里的 JSONB 字节数（u32，<= 65535）
 *
 * 【为什么不用"长度 > 2048 就是标记"这种隐式约定】
 * 那依赖一个跨层的隐式契约。显式 tag 更稳：读侧先看 tag，
 * 对不上就当普通内联 JSONB 处理。
 *
 * 【为什么 tag 首字节不可能撞上合法 JSONB】
 * 合法 JSONB tag 只有 0..8（见 modalities/doc/jsonb.h），而 0x4A 不在其中。
 * 所以"12 字节既能是标记又是合法 JSONB"在字节层面不存在 —— 零歧义。
 *
 * 【为什么 chain_len 必须持久化】
 * 读路径原本要两趟 walk（先量长度、再拷内容），两趟之间若链被 VACUUM 释放，
 * 第二趟会读到已清零的页。长度存进 tuple 之后只走一趟，窗口消失。
 *
 * ⚠️ 标记本身 12 字节，但**链的内容不进 tuple** ——
 *    所以 64KB 文档不会把 tuple 撑爆（tuple 各层长度都是 u16）。
 */
#define REC_JSON_OVF_TAG   0x4A534F56u
#define REC_JSON_OVF_MARK_SIZE 12u

/* 这段字节是不是一个 JSON 溢出标记（只做字节判别，不校验链） */
bool rec_json_ovf_is_marker(const u8 *bytes, u32 len);

/*
 * 解析溢出标记。
 *
 * 返回 OK 并把 head / chain_len 写出来；
 * 不是标记、head 非法、或 chain_len 超过 65535 ⇒ E_INVAL。
 *
 * ⚠️ 不检查页链本身是否存在 —— 那是读路径（json_ovf_read_once）的事。
 */
int rec_json_ovf_decode(const u8 *bytes, u32 len,
                        u32 *head_out, u32 *len_out);

/*
 * 把溢出标记写进一段缓冲（调用方保证 cap >= REC_JSON_OVF_MARK_SIZE）。
 */
int rec_json_ovf_encode(u32 head, u32 chain_len, u8 *buf, u16 cap);

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
