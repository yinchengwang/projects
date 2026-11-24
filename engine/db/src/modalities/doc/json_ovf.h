#ifndef DB_MODALITIES_DOC_JSON_OVF_H
#define DB_MODALITIES_DOC_JSON_OVF_H

/*
 * json_ovf.h —— 大 JSON 文档的外置页链（v1.6 步骤 3 · 判决 3）
 *
 * ==================== 这是"外部值"的第 3 个用户 ====================
 *
 * 外部值机制（src/storage/value.h）已经有两个用户：
 *   1. blob   —— 按**帧**读（ext_key = blob_id，ext_off = 帧号）
 *   2. vector —— 按整体读（ext_key = row_id，ext_off = 0）
 * 3. **大 JSON** ← 本文件（按**页链**读）
 *
 * ⭐ **契约一行不改**：外部值的 `ext_fn` 约定是
 *
 *     int (*DetoastFn)(void *ctx, u64 key, u64 off,
 *                      u8 *buf, u16 cap, u16 *len_out);
 *
 *   - 第一次调用（buf == NULL, cap == 0）只报长度；
 *   - 第二次调用（buf != NULL）把内容读进来；
 *   - 缓冲不够 ⇒ 返回 E_NOSPACE。
 *
 * 本文件**不碰**这个契约，只提供"怎么按页链读"这一件事。
 *
 * ==================== 为什么大文档要外置 ====================
 *
 * heap 行有大小限制（一条行放不进一页就得跨页，代价高）。
 * 而 JSON 文档的大小**极不均匀**（几百字节到几十兆都有）。
 * ⇒ 小文档内联进行的列值；**超过阈值的走外置**。
 *
 * 阈值（内联上限）由本层自己定，但**必须**：
 *   - 明显小于页大小（否则内联的文档会把行撑爆）；
 *   - 明显大于"常见小文档"（否则大部分文档都要走页链，慢）。
 * 建议 4KB 的一半左右 —— 具体数字由实现定，但要写进常量并可测。
 *
 * ==================== 页链格式 ====================
 *
 *   PT_JSON_OVF 页（页类型 0x1B）：
 *     偏移 0   u32  next（下一页的页号；PAGE_ID_NIL 表示链尾）
 *     偏移 4   u32  payload_len（本页有效字节数；**除**页头外的字节数）
 *     偏移 8   u8   payload[PAGE_SIZE - 8]
 *
 * ⚠️ **页头是 8 字节**：next(4) + payload_len(4)。
 * 内容按页存放，**最后一页可能不满** ⇒ 用 payload_len 截断。
 *
 * ⚠️ **实际的字节偏移不是 0/4/8（坑 M7-25）**
 *
 * 上面那三行是"JSON 自己的页头"的**结构**（8 字节：next + payload_len），
 * 但它**不能放在偏移 0**：偏移 0..19 是标准 slotted 页头
 * （H_TYPE @0、H_CKSUM @16…），而本文件又要求"每一页都要校验
 * （页类型 + 校验和）" ⇒ 页类型与校验和必须留在标准页头里。
 *
 * ⇒ 实际布局（json_ovf.c 是权威）：
 *
 *     偏移 0..19    标准 slotted 页头（H_TYPE = PT_JSON_OVF，H_CKSUM = 校验和）
 *     偏移 20..27   JSON 页头：next(4) + payload_len(4)
 *     偏移 28..     payload（每页最多 4068 字节）
 *
 * 代价是每页少 20 字节可用空间；换来的是"页类型 + 校验和"能落地。
 * （备选：① next 借用标准页头的 H_NEXT、payload_len 放 20 ⇒ 容量 4072，
 *   但 8 字节头不连续；② 照字面把 payload 放偏移 8 ⇒ 盖掉 H_CKSUM，做不到。）
 *
 * ⚠️ **链上每一页都要校验**（页类型 + 校验和）——
 * 不校验就是"读到坏数据还不报错"。
 *
 * ==================== 与 blob 的差别 ====================
 *
 * blob 的帧是**定长**的（帧号 → 偏移是纯算术），
 * 页链的最后一页**可能不满** ⇒ 要靠 payload_len 截断。
 * 这是"页链读"与"按帧读"的唯一实质差别。
 */

#include "common/error.h"
#include "common/loghook.h"
#include "common/types.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 页类型（全局页类型表的下一个） */
#define PT_JSON_OVF 0x1Bu

/* 页头大小 */
#define JSON_OVF_HDR 8u

/*
 * 内联阈值：记录内超过这个大小就写入 PT_JSON_OVF 页链。
 *
 * ⚠️ 这个 2048 与**值层**的内联上限（VAL_INLINE_MAX = 255，见
 *    storage/value.h）不是同一个数 —— 两者之间的文档在 heap 记录里
 *    内联，但在运行时值中用指向当前记录缓冲的通用外部值表示，
 *    因而不会被文本路径截断。两个阈值服务两个不同的目的，别合并。
 */
#ifndef JSON_INLINE_MAX
#define JSON_INLINE_MAX 2048u
#endif

/*
 * 页内布局（见上面的坑 M7-25；json_ovf.c 是权威）。
 *
 * JSON 页头紧跟在标准 slotted 页头之后，**8 字节是连续的**
 * （next(4) + payload_len(4)）—— 只是整体后移了 HDR_SIZE。
 */
#define JSON_OVF_HDR_OFF  ((u32)HDR_SIZE)             /* 20：JSON 页头的起点 */
#define JSON_OVF_NEXT_OFF (JSON_OVF_HDR_OFF + 0u)      /* 20：u32 next */
#define JSON_OVF_LEN_OFF  (JSON_OVF_HDR_OFF + 4u)      /* 24：u32 payload_len */
#define JSON_OVF_DATA_OFF (JSON_OVF_HDR_OFF + JSON_OVF_HDR) /* 28：payload 起点 */
#define JSON_OVF_PAY_CAP  (PAGE_SIZE - JSON_OVF_DATA_OFF)  /* 4068：每页容量 */

/*
 * 单份文档的长度上限：**65535 字节**。
 *
 * ⚠️ 这是"契约一行不改"的直接后果（坑 M7-28）：
 * DetoastFn 的长度出口只有 u16（见 src/storage/value.h）⇒
 * 超过 65535 字节的文档**没法被物化**（val_detoast 拿到的是截断的长度）。
 *
 * 本层的选择：**报错**（json_ovf_write 返回 E_NOSPACE），
 * 既不截断（截断 = 静默给错答案），也不静默接受（接受 = 后面物化时炸）。
 *
 * ⚠️ 请上层裁决（交付说明里已提请 lead 判决）：
 *   ① 把"单文档 ≤ 64KB"作为文档模态的正式限制写进公开语义；
 *   ② 还是要更大文档 ⇒ 必须动 DetoastFn（分块读），那就不是"契约一行不改"了。
 */
#define JSON_OVF_MAX_LEN  65535u

/*
 * 判断要不要外置。
 *
 * ⚠️ 这个判断**必须在写入路径与读取路径用同一个函数** ——
 * 两边各判一次，早晚有一边不一致 ⇒ 读的时候要么"外置的当内联读"
 * （读到垃圾）要么"内联的去页链找"（找不到）。
 */
bool json_should_ovf(u32 len);

typedef struct {
    LogHookFn fn;
    void *ctx;
    u32 xid;
} JsonOvfWal;

/*
 * 把一份内容写成页链。
 *
 * wal == NULL 只保留给旧的独立页链单测；生产写路径必须传 WAL 上下文。
 * 每页的最终内容只写一次，调用 wal->fn 前不改页，避免恢复 undo 的
 * before 镜像出现"先写载荷、后回填 next"的半成品状态。
 */
int json_ovf_write(Pager *pg, const JsonOvfWal *wal,
                   const u8 *data, u32 len, u64 *ext_key_out);

/*
 * 按页链读一份内容。
 *
 * ⚠️ **契约与 DetoastFn 一模一样**（两次调用：先问长度、再取内容）——
 * 这是"契约一行不改"的落点。
 *
 * buf == NULL ⇒ 只报长度（len_out）。
 * buf != NULL ⇒ 读进来；cap 不够 ⇒ E_NOSPACE（并报出需要多少）。
 *
 * ⚠️ 读路径分两趟 walk（先量长度、再拷内容），**两趟之间没有链级快照**。
 *    因此同一 ext_key 的 read 必须与 write/free 由调用方串行化，
 *    且 free 之前必须确保没有读者仍在执行。
 */
int json_ovf_read(Pager *pg, u64 ext_key, u8 *buf, u16 cap, u16 *len_out);

/*
 * 释放一条页链（把页还给全局页分配器）。
 *
 * ⚠️ 释放之前调用方必须确保**没有人在读这条链**。
 *    本层不等待读者，也不做引用计数 —— 生命周期由调用方锁负责。
 */
int json_ovf_free(Pager *pg, u64 ext_key);

/* 统计一条链有几个页（诊断与测试用） */
int json_ovf_page_count(Pager *pg, u64 ext_key, u32 *n_out);

/*
 * DetoastFn 适配器（签名见 src/storage/value.h 的 DetoastFn）。
 *
 * json_ovf_read 的参数是 (Pager *, ext_key)，而 DetoastFn 是
 * (ctx, key, off, buf, cap, len_out) ⇒ 需要一个适配器把两者接上。
 *
 * 句柄：key = 页链首页的页号（就是外部值的 ext_key），off = 0
 *      —— 一个外置文档是一个整体，没有"偏移"可言（与 vector 同一口径）。
 *
 * 用法：
 *     val_external_text(&v, ext_key, 0, json_ovf_detoast, pg);
 *
 * ⚠️ 语义与 json_ovf_read 完全一致：两次调用（先问长度、再取内容），
 * 缓冲不够 ⇒ E_NOSPACE 并报出需要多少字节。
 */
int json_ovf_detoast(void *ctx, u64 key, u64 off,
                     u8 *buf, u16 cap, u16 *len_out);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_DOC_JSON_OVF_H */
