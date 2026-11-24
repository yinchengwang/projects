/*
 * json_ovf_read.c —— 按页链读回内容（v1.6 C5.3 · C5.3b-b）
 *
 * ==================== 为什么单独一个文件 ====================
 *
 * `json_ovf.c` 承担写入与释放，已经贴到 800 行硬上限。
 * 而"读"与"写"本来就是两件事：
 *   - 写：分配页 → 定页序 → 每页只写一次最终内容 → 记 WAL
 *   - 读：沿 next 走 → 校验每页 → 按长度拷出
 *
 * 拆开之后两边都不用为了对方的细节读整份文件。
 *
 * ==================== 三个读入口的区别 ====================
 *
 *   json_ovf_read       老路径：两趟 walk（先量长度、再拷内容）
 *   json_ovf_read_len   只报长度：有已知长度就直接报，不碰页
 *   json_ovf_read_once  用已知长度一趟读完，并交叉校验
 *
 * 【为什么要"已知长度"这一档】
 * 两趟 walk 的两趟之间若链被释放（VACUUM 回收已删版本），
 * 第二趟读到的是已清零的页。把长度持久化进 tuple 之后，
 * 长度不必走过去量 ⇒ 只走一趟，那条竞态窗口随之消失。
 *
 * ⚠️ 拆文件后 `json_ovf_walk` 不再是 static：它被本文件的三个读入口
 *    和 json_ovf.c 的 free/page_count 共用。声明在本文件顶部，
 *    不属于公开 API（json_ovf.h 里没有它）。
 */

#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/doc/json_ovf.h"
#include "storage/page.h"
#include "storage/pager.h"


/*
 * 走一条页链。
 *
 * buf == NULL ⇒ 只统计长度与页数（"问长度"那一趟）。
 * buf != NULL ⇒ 顺手把内容拷进 buf（cap 不足 ⇒ E_NOSPACE）。
 * ids != NULL ⇒ 把链上每一页的页号记进 ids（释放前先看清全链用）。
 *
 * ⚠️ 每一页都要校验：
 *   - 页类型必须是 PT_JSON_OVF（不是 ⇒ 这页不是我们的）；
 *   - 校验和必须对（对不上 ⇒ 页被改坏了/没落盘完整）；
 *   - payload_len 不得超过页的实际容量；
 *   - 非末页必须装满。
 * 不校验就是"读到坏数据还不报错"。
 */
int json_ovf_walk(Pager *pg, u32 first, u8 *buf, u16 cap,
                         u32 *total_out, u32 *pages_out, u32 *ids)
{
    u32 pid = first;
    u32 total = 0;
    u32 seen = 0;

    if (total_out)
        *total_out = 0;
    if (pages_out)
        *pages_out = 0;

    while (pid != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        u32 next;
        u32 plen;
        int rc;

        /* 闸门：链比这还长 ⇒ 成环了（或者链被改坏了） */
        if (seen >= JSON_OVF_MAX_PAGES)
            return E_CORRUPT;

        rc = pager_get(pg, pid, &fr);
        if (rc != OK)
            return rc;      /* 链被截断：next 指到了不存在的页 */

        if (fr->data[H_TYPE] != (u8)PT_JSON_OVF ||
            !page_verify_checksum(fr->data)) {
            (void)pager_unpin(pg, pid, false);
            return E_CORRUPT;
        }

        next = get_u32(fr->data + JSON_OVF_NEXT_OFF);
        plen = get_u32(fr->data + JSON_OVF_LEN_OFF);

        /* payload_len 超过页的实际容量 ⇒ 这页的描述是假的 */
        if (plen > JSON_OVF_PAY_CAP) {
            (void)pager_unpin(pg, pid, false);
            return E_CORRUPT;
        }

        /* 非末页必须装满（只有末页可能不满，靠 payload_len 截断） */
        if (next != PAGE_ID_NIL && plen != JSON_OVF_PAY_CAP) {
            (void)pager_unpin(pg, pid, false);
            return E_CORRUPT;
        }

        /* 总长超过 u16 ⇒ 这条链根本没法被物化出来 */
        if (total + plen > JSON_OVF_MAX_LEN) {
            (void)pager_unpin(pg, pid, false);
            return E_CORRUPT;
        }

        if (buf != NULL) {
            if (total + plen > (u32)cap) {
                (void)pager_unpin(pg, pid, false);
                if (total_out)
                    *total_out = total;
                return E_NOSPACE;
            }
            if (plen)
                memcpy(buf + total, fr->data + JSON_OVF_DATA_OFF, plen);
        }

        total += plen;
        if (ids != NULL)
            ids[seen] = pid;

        rc = pager_unpin(pg, pid, false);
        if (rc != OK)
            return rc;

        seen++;
        pid = next;
    }

    if (total_out)
        *total_out = total;
    if (pages_out)
        *pages_out = seen;
    return OK;
}

/* ---------------- 读取 ---------------- */

/*
 * 按页链读一份内容。
 *
 * ⚠️ **契约与 DetoastFn 一模一样**（见 src/storage/value.h）：
 *   - buf == NULL ⇒ 只报长度（len_out）；
 *   - buf != NULL ⇒ 读进来；cap 不够 ⇒ E_NOSPACE（并报出需要多少）。
 *
 * 与 vector 的 vec_detoast（modalities/vector/vector_am.c）同一个口径：
 * cap 不够时**不要写 buf**，只填 len_out 并返回 E_NOSPACE。
 */
int json_ovf_read(Pager *pg, u64 ext_key, u8 *buf, u16 cap, u16 *len_out)
{
    u32 total = 0;
    u32 pages = 0;
    int rc;

    if (!pg || !len_out)
        return E_INVAL;
    if (ext_key > 0xFFFFFFFFu)
        return E_INVAL;

    /* 第一趟：问长度（坑 M7-31：读两趟是两次调用约定的必然代价） */
    rc = json_ovf_walk(pg, (u32)ext_key, NULL, 0, &total, &pages, NULL);
    if (rc != OK)
        return rc;

    if (buf == NULL) {
        *len_out = (u16)total;
        /* 与 vec_detoast 同款：cap 不够（这里恒为 0）⇒ E_NOSPACE + 报长度 */
        return (total > 0) ? E_NOSPACE : OK;
    }

    if ((u32)cap < total) {
        *len_out = (u16)total;
        return E_NOSPACE;
    }

    /* 第二趟：把内容拷出来 */
    {
        u32 got = 0;
        u32 got_pages = 0;

        rc = json_ovf_walk(pg, (u32)ext_key, buf, cap, &got, &got_pages, NULL);
        if (rc != OK)
            return rc;
        *len_out = (u16)got;
    }

    return OK;
}

/* ---------------- 用已知长度的一趟读取 ---------------- */

/*
 * 为什么需要"已知长度"的读入口
 *
 * 老读路径是两趟 walk：第一趟沿链走出总长度，第二趟再沿链拷内容。
 * 两趟之间链**没有任何保护** —— 若此刻有别的执行流释放了这条链
 * （VACUUM 回收已删版本的链），第二趟读到的就是已清零的页，
 * 结果是 E_CORRUPT 甚至脏数据。
 *
 * 把链的总长度持久化在 tuple 里（record 的溢出标记，见设计 §4.2）之后，
 * 长度就不再需要"走过去量"了 ⇒ 读路径只走一趟，
 * 两趟之间那条竞态窗口随之消失。
 */

/*
 * 只问长度。
 *
 * chain_len != 0 ⇒ 用它校验链；chain_len == 0 ⇒ 沿链走一遍量出来。
 *
 * ⚠️ 即使有已知长度也走一遍链：长度是"期望值"，链可能被改坏。
 *    这里只统计、不拷贝 —— 代价一次遍历，换来"坏链当场报错"。
 */
int json_ovf_read_len(Pager *pg, u64 ext_key, u32 chain_len, u32 *len_out)
{
    u32 total = 0;
    u32 pages = 0;
    int rc;

    if (!pg || !len_out)
        return E_INVAL;
    if (ext_key > 0xFFFFFFFFu)
        return E_INVAL;
    if (chain_len > JSON_OVF_MAX_LEN)
        return E_NOSPACE;

    /*
     * 有已知长度 ⇒ **直接报它，不碰页**。
     *
     * 【为什么这里可以信任 chain_len】
     * 它的用途只是"给调用方一次分配缓冲的大小"。真正的校验发生在
     * json_ovf_read_once：那里会把实际链路长度与 chain_len 对比，
     * 不符就返回 E_CORRUPT。如果这里也走一遍链，就又把"两次 walk"
     * 请回来了 —— 那正是这个 API 存在的意义所在。
     */
    if (chain_len != 0) {
        *len_out = chain_len;
        return OK;
    }

    /* 没给长度 ⇒ 只能走一遍量出来（老路径） */
    rc = json_ovf_walk(pg, (u32)ext_key, NULL, 0, &total, &pages, NULL);
    if (rc != OK)
        return rc;

    *len_out = total;
    return OK;
}

/*
 * 用已知长度读。
 *
 * chain_len != 0 ⇒ 按它校验并在链路长度不符时返回 E_CORRUPT；
 * chain_len == 0 ⇒ 退化成老的两趟 walk（兼容未接线的调用点）。
 *
 * 契约与 DetoastFn 一致：
 *   buf == NULL ⇒ 只报长度，返回 E_NOSPACE（与 json_ovf_read 同款）；
 *   buf != NULL 且 cap 不够 ⇒ E_NOSPACE 并报出需要多少。
 */
int json_ovf_read_once(Pager *pg, u64 ext_key, u32 chain_len,
                       u8 *buf, u16 cap, u16 *len_out)
{
    u32 total = 0;
    u32 pages = 0;
    int rc;

    if (!pg || !len_out)
        return E_INVAL;
    if (ext_key > 0xFFFFFFFFu)
        return E_INVAL;
    if (chain_len > JSON_OVF_MAX_LEN)
        return E_NOSPACE;

    if (chain_len == 0) {
        /* 没有已知长度 ⇒ 走老路径（内部就是两趟 walk） */
        return json_ovf_read(pg, ext_key, buf, cap, len_out);
    }

    rc = json_ovf_walk(pg, (u32)ext_key, buf, cap, &total, &pages, NULL);
    if (rc != OK)
        return rc;

    if (total != chain_len) {
        /*
         * 链的实际长度与 tuple 记的不一致。
         *
         * 两种可能：① 链被改坏；② tuple 的长度字段写错了。
         * 无论哪种，都不能把读到的内容当答案交出去 ——
         * 半个文档比报错危险得多。
         */
        return E_CORRUPT;
    }

    *len_out = (u16)total;
    if (buf == NULL)
        return (total > 0) ? E_NOSPACE : OK;
    return OK;
}

/* ---------------- 统计 ---------------- */

/* 统计一条链有几个页（诊断与测试用） */
int json_ovf_page_count(Pager *pg, u64 ext_key, u32 *n_out)
{
    u32 total = 0;
    u32 pages = 0;
    int rc;

    if (!pg || !n_out)
        return E_INVAL;
    if (ext_key > 0xFFFFFFFFu)
        return E_INVAL;

    rc = json_ovf_walk(pg, (u32)ext_key, NULL, 0, &total, &pages, NULL);
    if (rc != OK)
        return rc;

    *n_out = pages;
    return OK;
}

/* ---------------- 外部值适配器 ---------------- */

/*
 * DetoastFn 适配器（签名见 src/storage/value.h）。
 *
 * 句柄：key = 页链首页的页号（ext_key），off = 0
 *      —— 一个外置文档是一个整体，没有"偏移"可言（与 vector 同一口径）。
 *
 * 用法：
 *     val_external_text(&v, ext_key, 0, json_ovf_detoast, pg);
 */
int json_ovf_detoast(void *ctx, u64 key, u64 off,
                     u8 *buf, u16 cap, u16 *len_out)
{
    if (!ctx)
        return E_INVAL;

    /*
     * off 是**已知的链总长度**（来自 tuple 的溢出标记）。
     *
     * 【为什么保留 off=0 的老语义】
     * 早期调用点（test_json_ovf 与未接线的路径）传 off=0，表示"我不知道长度"。
     * read_once 把 chain_len==0 解释成"退化成 walk"，所以老调用仍然正确。
     *
     * 【为什么不用 off==0 当非法句柄】
     * 空文档的合法长度也是 0。用 0 当哨兵会把"空文档"和"没给长度"
     * 混成一件事。这里选择"0 = 没给长度、走 walk" —— 空文档走 walk
     * 也能正确得到 0（一个 payload_len=0 的页），两种语义不冲突。
     */
    if (off > JSON_OVF_MAX_LEN)
        return E_NOSPACE;

    /*
     * 第一趟（buf == NULL）：已知长度就直接报，**不碰页**。
     *
     * 这是"长度持久化在 tuple 里"换来的真正收益 ——
     * val_detoast 先问长度那一步不再需要走一遍链。
     */
    if (buf == NULL && off != 0) {
        *len_out = (u16)off;
        return E_NOSPACE;
    }

    return json_ovf_read_once((Pager *)ctx, key, (u32)off, buf, cap, len_out);
}
