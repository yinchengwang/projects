/*
 * record_json_ovf.c —— JSON 溢出标记的字节级判别（C5.3b-b）
 *
 * ==================== 为什么单独一个文件 ====================
 *
 * `record.c` 是 tuple 布局与编解码（282 行，职责很集中）。
 * "JSON 溢出标记"是一个**独立的字节协议**：它只关心"这 12 个字节
 * 是不是标记、指向哪条链、链有多长"，和 tuple 布局没有关系。
 *
 * 放在 record.c 里会让那个文件同时承担两件不相干的事；
 * 单独一个文件之后，两边都可以按自己的语义演进。
 *
 * ==================== 这一层不做什么 ====================
 *
 * - 不读页链（那是 json_ovf_read_once 的事）
 * - 不判断该不该外置（那是 json_should_ovf 的事）
 * - 不认识 JSONB 的内容（只知道长度上限）
 *
 * 它只做一件事：**12 个字节 ↔ (tag, head, chain_len)**。
 * 这样 record.c 的编解码只需要问"这段字节像不像标记"，
 * 不需要把 tag 常量与页号语义搬进 tuple 布局那一层。
 */

#include "storage/record.h"

#include <string.h>

/* 单个 JSON 文档的字节上限（与 modalities/doc/json_ovf.h 的
 * JSON_OVF_MAX_LEN 同值；这里独立定义一个常量，避免 L1 include L4）。
 *
 * ⚠️ 两个数字必须一致 —— 它们是同一个物理约束（DetoastFn 的长度出口是 u16）
 *    在两个层次上的表述。改了 JSON_OVF_MAX_LEN 就要同步这里。
 *    这条纪律与"编码与解码必须用同一个 col_is_var"同源。 */
#define REC_JSON_OVF_MAX_LEN 65535u

/* 页号 0 是合法页号（文件第一页），拿它当哨兵会真的撞上 ——
 * 与 json_ovf.h 的坑 M7-32 同一条纪律。PAGE_ID_NIL 的定义在 pager.h，
 * 但 record 层不认识 pager；这里用同一个值（0xFFFFFFFFu）并写明来源。 */
#define REC_PAGE_ID_NIL 0xFFFFFFFFu

bool rec_json_ovf_is_marker(const u8 *bytes, u32 len)
{
    if (bytes == NULL || len != REC_JSON_OVF_MARK_SIZE)
        return false;

    /* 小端读 u32：与 tuple 里其它数值一致（DESIGN.md §3.1） */
    u32 tag = (u32)bytes[0]
            | ((u32)bytes[1] << 8)
            | ((u32)bytes[2] << 16)
            | ((u32)bytes[3] << 24);

    return tag == REC_JSON_OVF_TAG;
}

int rec_json_ovf_decode(const u8 *bytes, u32 len,
                        u32 *head_out, u32 *len_out)
{
    u32 head;
    u32 clen;

    if (head_out)
        *head_out = 0;
    if (len_out)
        *len_out = 0;

    if (!rec_json_ovf_is_marker(bytes, len))
        return E_INVAL;

    head = (u32)bytes[4]
         | ((u32)bytes[5] << 8)
         | ((u32)bytes[6] << 16)
         | ((u32)bytes[7] << 24);
    clen = (u32)bytes[8]
         | ((u32)bytes[9] << 8)
         | ((u32)bytes[10] << 16)
         | ((u32)bytes[11] << 24);

    /*
     * 两条都必须拒：
     *   - head == PAGE_ID_NIL ⇒ 标记指向空处（空文档也有真实存在的链页）
     *   - clen > 65535        ⇒ 超过 DetoastFn 能表达的长度，读不出来
     *
     * 任何一种放过去，读的时候才发现 —— 那时已经在别处报 E_CORRUPT，
     * 定位成本高得多。
     */
    if (head == REC_PAGE_ID_NIL)
        return E_INVAL;
    if (clen > REC_JSON_OVF_MAX_LEN)
        return E_INVAL;

    if (head_out)
        *head_out = head;
    if (len_out)
        *len_out = clen;
    return OK;
}

int rec_json_ovf_encode(u32 head, u32 chain_len, u8 *buf, u16 cap)
{
    u32 tag = REC_JSON_OVF_TAG;

    if (!buf || cap < (u16)REC_JSON_OVF_MARK_SIZE)
        return E_NOSPACE;
    if (head == REC_PAGE_ID_NIL)
        return E_INVAL;
    if (chain_len > REC_JSON_OVF_MAX_LEN)
        return E_NOSPACE;

    buf[0] = (u8)(tag & 0xFFu);
    buf[1] = (u8)((tag >> 8) & 0xFFu);
    buf[2] = (u8)((tag >> 16) & 0xFFu);
    buf[3] = (u8)((tag >> 24) & 0xFFu);

    buf[4] = (u8)(head & 0xFFu);
    buf[5] = (u8)((head >> 8) & 0xFFu);
    buf[6] = (u8)((head >> 16) & 0xFFu);
    buf[7] = (u8)((head >> 24) & 0xFFu);

    buf[8] = (u8)(chain_len & 0xFFu);
    buf[9] = (u8)((chain_len >> 8) & 0xFFu);
    buf[10] = (u8)((chain_len >> 16) & 0xFFu);
    buf[11] = (u8)((chain_len >> 24) & 0xFFu);

    return OK;
}
