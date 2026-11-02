#ifndef DB_STORAGE_BATCH_H
#define DB_STORAGE_BATCH_H

#include "common/config.h"
#include "common/types.h"
#include "storage/record.h"
#include "storage/value.h"

/*
 * 批接口的契约类型（v1.2 步骤 0）。
 *
 * ==================== 为什么这个文件在 storage/（L1） ====================
 *
 * 和 DbValue 是同一个理由（见 storage/value.h 顶部的说明）：
 * 列批是**存储层、查询层、SPI 三方共用的契约语言**。
 *
 *   - 模态（spi/ 的 AM 实现）往列数组里写
 *   - 执行器（query/）从列数组里读
 *   - 两边要看到**同一套内存布局**，否则就是"看起来一样、读出来是乱的"
 *
 * 放在 query/ 会让 spi/ 反过来依赖 query/，层次就倒了；
 * 放在 storage/（L1）则 spi/ → storage/、query/ → storage/ 都成立 ✓
 *
 * ==================== 它解决什么问题 ====================
 *
 * v1.1 把执行器批式化了，但实测只拿到 1.11×（目标 3×）。
 * 原因不是求值不够快，而是**瓶颈在 SPI 的逐行接口**：
 *
 *   | 逐行 scan_next | ~38% | 执行器只能一行一行攒批 |
 *   | 逐行 decode     | ~43% | 每行都要物化一个 272 字节的 DbValue |
 *
 * 列式求值省的是"过滤时逐列 val_copy"，而那部分已经被 v1.1 吃掉了 ——
 * **要省的是物化，而物化在 SPI 的 decode 里。**
 *
 * 所以本文件定义两个新能力（PLAN-v1.2 §1.1.1）：
 *   - AM_CAP_BATCH_SCAN   一次给一批原始行
 *   - AM_CAP_BATCH_DECODE 一次解一批，直接进列数组，不物化 DbValue
 *
 * 走能力协商：caps 位 + 函数指针为 NULL = 不支持，执行器自动退回逐行。
 * **blob / graph / vector 三个已验收的模态一行都不用改** ✓
 */

#define AM_BATCH_MAX_ROWS VEC_BATCH   /* 一批最多多少行（与执行器的批宽一致） */

/*
 * 批扫描的行字节竞技场大小。
 *
 * 【为什么要竞技场，而不是"AM 给我一批指针"】
 *
 * 行是**变长**的（heap 的行长随列内容变）。AM 没法凭空造出一块
 * 连续的、装着 N 个变长行的内存 —— 除非它自己有一块。
 *
 * 竞技场 = 扫描状态自己的一块缓冲，把这一批的行字节**依次码进去**。
 * 行多小就装多少行；装满了这一批就到此为止（返回的行数少于请求值）。
 *
 * 256 KB 的取舍：
 *   - 行典型大小几十字节 → 装得下上千行（批宽上限 1024 才是瓶颈）
 *   - 行最大 4 KB（PAGE_SIZE）→ 至少装得下 64 行，不会出现"一行都装不下"
 */
#define AM_BATCH_ARENA_BYTES (256u * 1024u)

/*
 * 一行的引用（原始字节串）。
 *
 * 【生命周期】
 * data 指向的字节由 AM 持有，**有效期到下一次 batch_scan 调用为止**。
 * 调用方必须在下一次 batch_scan 之前把数据用掉（解码 / 拷贝走）。
 */
typedef struct {
    const u8 *data;
    u16      len;
    Rid      rid;
} AmRowRef;

/*
 * 外部值句柄（blob 模态的"内容不在库里，只记去哪儿取"）。
 *
 * 与 DbValue 里的外部值字段一一对应 —— 批里存的是同样的四样东西。
 */
typedef struct {
    u64       key;
    u64       off;
    DetoastFn fn;
    void     *ctx;
} AmExtRef;

/*
 * 一列的批数据。
 *
 * 【为什么按类型分开存，而不是"每列一个 DbValue 数组"】
 *
 * DbValue 一个 272 字节。一行四列 = 1088 字节，一批 1024 行 = 1.1 MB ——
 * 逐行过滤时在这 1.1 MB 里跳着读，全在 L2/L3 之间来回。
 *
 * 列式：一列一个紧凑数组（INT 列每行 8 字节），一批 1024 行 = 8 KB，全在 L1 里。
 *
 * 【与执行器的批是同一套类型】
 *
 * 执行器（query/exec_batch.h）直接用本类型，不另起一套 ——
 * 两套布局意味着"模态写进去的"和"执行器读出来的"可能不是一回事，
 * 而这种错**不会报错**，只会静默地读出乱值。
 */
typedef struct {
    u8       type;      /* ColType：这一列的类型（一批内不变） */
    bool     used;      /* 这一列要不要进批（只进用得上的列） */
    u16      n;         /* 本批的行数 */
    u8      *nulls;     /* NULL 位图 */
    i64     *ints;      /* T_INT4 / T_INT8 / T_BOOL */
    double  *reals;     /* T_REAL */
    u32     *toff;      /* T_TEXT（内联）：文本池里的偏移 */
    u16     *tlen;      /* T_TEXT（内联）：长度 */
    u8      *tpool;     /* 文本池 */
    u32      tused;
    u32      tcap;
    u8      *extflag;   /* 位图：1 = 该行是外部值 */
    AmExtRef *exts;      /* 外部句柄（用到才分配） */
} AmBatchCol;

/*
 * 一批。
 *
 * cols 是**调用方分配**的列数组（长度 ncols）。
 * 为什么不是定长数组：列数是表的属性，SPI 这一层不该假设一个上限
 * （定长数组会引入一个假的限制）。
 *
 * nrows 的角色有双重性：
 *   - 调用方读：这一批已经有多少行
 *   - 模态写：**写游标** —— 批解码从第 nrows 行开始写，写完前进 nrows 行
 */
typedef struct {
    int         ncols;
    u16         nrows;
    AmBatchCol *cols;
} AmBatch;

/* ---------------- 位图小工具 ---------------- */

static inline void bit_set(u8 *bm, u32 i)
{
    bm[i >> 3] |= (u8)(1u << (i & 7u));
}

static inline bool bit_get(const u8 *bm, u32 i)
{
    return ((bm[i >> 3] >> (i & 7u)) & 1u) != 0;
}

/*
 * 批解码：把一批记录直接解进列数组，**不物化 DbValue**。
 *
 * 这是 SPI 批接口的核心：省掉"每行一个 272 字节 DbValue"的中间层。
 *
 * L       预计算的记录布局（AM 每条查询算一次）
 * cols    列定义（AM 持有）
 * rows    这一批的原始行
 * out     目标列批；out->nrows 是写游标（调用前为 0）
 *
 * 只写 out->cols[c].used 为真的列 —— "这一列用不上就不用解"，
 * 和执行器"只把用得上的列放进批"是同一个决定。
 *
 * 失败返回错误码；out->nrows 保持不变（半截的批次不能算成功）。
 */
int val_batch_decode(const RecLayout *L, const ColDesc *cols, int ncols,
                     const AmRowRef *rows, int nrows, AmBatch *out);

#endif /* DB_STORAGE_BATCH_H */
