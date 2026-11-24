/*
 * json_ovf.c —— 大 JSON 文档的外置页链（v1.6 步骤 3 · 判决 3）
 *
 * ==================== 这一层只做一件事 ====================
 *
 * 把"一份很大的 JSON 文档"变成"一条页链"，以及把这条页链原样读回来。
 *
 * 它**不**知道 JSON 是什么 —— 不解析、不建树、不做路径索引。
 * 那是 jsonb.c / json_path.c 的事。这里只管**字节**。
 *
 * ==================== 与外部值机制的关系 ====================
 *
 * 外部值的取回函数签名（见 src/storage/value.h 的 DetoastFn）：
 *
 *     int (*DetoastFn)(void *ctx, u64 key, u64 off,
 *                      u8 *buf, u16 cap, u16 *len_out);
 *
 * 约定是**两次调用**：第一次 buf == NULL 只报长度，
 * 第二次 buf != NULL 把内容读进来；缓冲不够返回 E_NOSPACE。
 *
 * 本文件的 json_ovf_read() 就是这个约定的**逐字实现**：
 *   - buf == NULL ⇒ 只报长度（写进 len_out）；
 *   - buf != NULL ⇒ 内容读进来；cap 不够 ⇒ E_NOSPACE（并报出需要多少）。
 *
 * 适配器 json_ovf_detoast() 把 (Pager *, ext_key) 翻译成 DetoastFn 的参数，
 * 于是调用方可以直接：
 *
 *     val_external_text(&v, ext_key, 0, json_ovf_detoast, pg);
 *
 * （与 src/spi/blob_am.c 里 val_external_text 的用法同款。）
 *
 * ==================== 页链格式 ====================
 *
 * 见 json_ovf.h。**本文件里的偏移是权威**：
 *
 *   偏移 0..19   标准 slotted 页头（H_TYPE / H_CKSUM / …）
 *   偏移 20..27  JSON 自己的 8 字节页头：next(4) + payload_len(4)
 *   偏移 28..    payload（每页最多 4068 字节）
 *
 * ==================== 坑 ====================
 *
 * 坑 M7-25：**8 字节的 JSON 页头不能放在偏移 0**。
 *   契约原本写的是"偏移 0 是 next / 偏移 4 是 payload_len / 偏移 8 是 payload"，
 *   但偏移 0 是标准 slotted 页头的 H_TYPE，偏移 16 是 H_CKSUM ——
 *   而契约自己又要求"每一页都要校验（页类型 + 校验和）"。
 *   两边对不上：payload 从偏移 8 开始会盖掉 H_NEXT / H_LSN / H_CKSUM / H_TID，
 *   校验和就**没地方存**，页类型也**没地方存**。
 *   ⇒ 本文件的取舍：**标准页头保留**（H_TYPE = PT_JSON_OVF，H_CKSUM 存校验和），
 *     JSON 自己的 8 字节页头整体后移到 HDR_SIZE（20），
 *     payload 从 28 开始 ⇒ 每页容量 4068 字节。
 *   （代价：每页少 20 字节可用空间；换来的是"页类型 + 校验和"能落地。
 *     备选方案见交付说明：① next 借用 H_NEXT、payload_len 放 20（容量 4072，
 *     但 8 字节头不连续）；② 照契约字面布局（做不到）。）
 *
 * 坑 M7-26：链上的 next **不保证页号单调**。
 *   pager_alloc 会**优先复用空闲页**（后进先出），复用的页号可能比前一页小。
 *   ⇒ 不能靠"页号严格递增"来防环。
 *   本文件的防环靠两件事：
 *     ① 写的时候每个页只被分配一次，next 只能指向**刚分配的、还没进链**的页
 *        ⇒ 结构上不可能成环；
 *     ② 读的时候有页数上限（JSON_OVF_MAX_PAGES）⇒ 万一链被改成环，
 *        也会在有限步内报 E_CORRUPT，而不是死循环。
 *
 * 坑 M7-27：链的总长**不得超过 u16**（65535）。
 *   DetoastFn 的长度出口只有 u16（见 value.h）⇒ 超过就是"没法物化"。
 *   写侧根本写不出来（见 M7-28）；读侧遇到这种链 ⇒ E_CORRUPT。
 *
 * 坑 M7-28：单份文档 > 65535 字节 ⇒ json_ovf_write 返回 **E_NOSPACE**。
 *   **不截断**（截断 = 静默给错答案，坑 M7-5 同一条纪律），
 *   **也不静默接受**（接受 = 后面物化的时候炸）。
 *   这是"契约一行不改"带来的硬限制 ⇒ 已在头文件里写明，请上层裁决。
 *
 * 坑 M7-29：释放页链时**先清零再归还**。
 *   pager_free 只把页号放回空闲栈，**不清内容**；下一个主人拿到的是
 *   "上一个主人的文档字节"。万一有人还拿着旧引用，读到的是**别人的数据**
 *   （静默错）。清零之后读到的是"空页"（页类型不对 ⇒ 报错）⇒ 宁可报错。
 *   （与 modalities/ts/ts_ttl.c 的 retire_page 同一个做法。）
 *
 * 坑 M7-30：**非末页的 payload_len 必须等于页容量**。
 *   只有最后一页可能不满（用 payload_len 截断）。
 *   中途出现"不满"的页 ⇒ 链是坏的 ⇒ E_CORRUPT。
 *
 * 坑 M7-31：读路径**走两趟**（先问长度，再拷内容）。
 *   这是 DetoastFn "两次调用"约定的必然代价：cap 不够时要报出需要多少，
 *   就必须先把整条链的长度算出来。⇒ 页读放大约 2 倍。
 *   （一次性把整条链读进临时缓冲可以只走一趟，但要额外分配，
 *     而"先问长度再读"是值层的既有约定，不该在这里另发明一套。）
 *
 * 坑 M7-32：空文档也**有明确表示** —— 一个 payload_len = 0 的页。
 *   不用"ext_key == 0 表示空"这种隐式约定：页号 0 是**合法的页号**
 *   （文件第一页），拿它当哨兵值 = 迟早有一天真的撞上。
 *
 * 坑 M7-33：这一层**没有 WAL 钩子**。
 *   外置页的改动目前不进日志 ⇒ 崩溃后"链写了一半"是可能的
 *   （读侧会报 E_CORRUPT，而不是给出错数据）。
 *   WAL 接线属于步骤 3 的后半（由 lead 做），本步骤不碰。
 *
 * 坑 M7-34：json_should_ovf 的阈值（JSON_INLINE_MAX = 2048）**大于**
 *   DbValue 的内联文本上限（VAL_TEXT_MAX = 256，即最多 255 字节）。
 *   ⇒ 256..2048 字节的文档按阈值判是"内联"，但值层的 s[] 装不下。
 *   本步骤只管外置层，不动阈值；但请上层在接线时裁决：
 *   要么把内联阈值降到 255，要么让 256..2048 的文档也走外置。
 */

#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/doc/json_ovf.h"
#include "storage/page.h"
#include "storage/pager.h"

/* 链上最多允许多少页 —— 既是防环闸门，也是防跑飞闸门 */
#define JSON_OVF_MAX_PAGES 64u

/* ---------------- 内部工具 ---------------- */

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
static int json_ovf_walk(Pager *pg, u32 first, u8 *buf, u16 cap,
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

/* ---------------- 判断要不要外置 ---------------- */

/*
 * 写入路径与读取路径必须用**同一个**判断函数 ——
 * 两边各判一次，早晚有一边不一致 ⇒ 读的时候要么"外置的当内联读"
 * （读到垃圾）要么"内联的去页链找"（找不到）。
 */
bool json_should_ovf(u32 len)
{
    return len > JSON_INLINE_MAX;
}

/* ---------------- 写入 ---------------- */

/*
 * 把一份内容写成页链。
 *
 * 页链是**新建**的（不接管已有链）。
 * 首页页号写进 ext_key_out —— 它就是外部值的 ext_key。
 *
 * ⚠️ 写的时候要"先分配、后回填 next"：
 *   下一张的页号在分配之前是不知道的。
 *   做法是：分配新页 ⇒ 回填**上一页**的 next ⇒ 重算上一页的校验和 ⇒ 放上一页。
 *   （上一页一直 pin 着，指针有效，不用重新取。）
 */
/*
 * 把一串页号逐页归还给全局页分配器。
 *
 * 【为什么不能复用 json_ovf_free】
 * json_ovf_free 是**按链走**的：它从首页出发沿 next 前进，
 * 一旦发现某页不是 PT_JSON_OVF 就判定链坏了并中止。
 * 而写入失败时，链的后半段**还没写过内容**（页是 pager_alloc 清零的），
 * 页头不是 PT_JSON_OVF ⇒ 按链走会在半路中止，后半段页泄漏。
 *
 * 写入路径自己记着完整页号列表，逐页归还才对。
 */
/*
 * 归还单个页号（不管它现在是 PT_JSON_OVF 还是全 0）。
 *
 * 语义与 json_ovf_free 的"逐页"部分一致：清零 → unpin → 进 freelist。
 * 不校验页类型 —— 调用方已经确认这一页是本次新建的。
 */
static int json_ovf_free_one(Pager *pg, u32 pid)
{
    PageFrame *fr = NULL;
    int rc;

    if (pid == PAGE_ID_NIL)
        return OK;

    rc = pager_get(pg, pid, &fr);
    if (rc != OK)
        return rc;

    memset(fr->data, 0, PAGE_SIZE);

    rc = pager_unpin(pg, pid, true);
    if (rc != OK)
        return rc;

    return pager_free(pg, pid);
}

static void ovf_free_page_list(Pager *pg, const u32 *ids, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        (void)json_ovf_free_one(pg, ids[i]);
    }
}

int json_ovf_write(Pager *pg, const JsonOvfWal *wal,
                   const u8 *data, u32 len, u64 *ext_key_out)
{
    u32 page_ids[JSON_OVF_MAX_PAGES];
    u8 zeros[PAGE_SIZE];
    u32 npages;
    u32 off = 0;
    u32 i;
    int rc = OK;

    if (!pg || !ext_key_out)
        return E_INVAL;
    if (len > 0 && !data)
        return E_INVAL;

    /* 坑 M7-28：单份文档不得超过 u16 能表达的长度 ⇒ 报错，不截断 */
    if (len > JSON_OVF_MAX_LEN)
        return E_NOSPACE;

    *ext_key_out = PAGE_ID_NIL;

    /* 坑 M7-32：空文档也占一个页（payload_len = 0），有明确表示 */
    npages = (len == 0)
                 ? 1u
                 : ((len + JSON_OVF_PAY_CAP - 1u) / JSON_OVF_PAY_CAP);
    if (npages > JSON_OVF_MAX_PAGES)
        return E_NOSPACE;

    /*
     * ============ 第一阶段：先把**所有**页号拿下 ============
     *
     * 【为什么不能边分配边写】
     * 页 i 的 next 指向页 i+1，而页 i+1 的页号在分配之前是未知的。
     * 老实现的办法是"分配页 i+1 → 回填页 i 的 next" —— 这让**页 i 被改两次**：
     *   ① 写载荷（next 暂填 PAGE_ID_NIL）
     *   ② 回填 next
     * 于是 WAL 里页 i 有两条 LOG_PAGEDATA，第二条的 before 镜像
     * 不是"全 0"，而是"载荷 + next=NIL"。undo 只会撤销第二条 ⇒
     * 结果是"首页还在、后面断了"的废链。
     *
     * 一次拿齐页号之后，每页的内容（载荷 + 最终 next）可以**一次算出来**，
     * 于是每页恰好一条 WAL 记录，before 恒为全 0 ⇒ undo 语义干净。
     *
     * ⚠️ 分配后立刻 unpin：17 页同时 pin 会白白占住帧池。
     *    丢 pin 之后页可能在第二阶段之前被逐出，第二阶段用 pager_get 重新取
     *    即可 —— 内容还没写过，取回来的就是 pager_alloc 清过的空页。
     */
    memset(zeros, 0, sizeof(zeros));
    for (i = 0; i < npages; i++) {
        PageFrame *fr = NULL;

        rc = pager_alloc(pg, &page_ids[i], &fr);
        if (rc != OK) {
            /* 回滚：把已经拿到的页号逐一还回去（不留下半条链） */
            ovf_free_page_list(pg, page_ids, i);
            return rc;
        }
        /* 立刻放开 pin：17 页同时 pin 会白白占住帧池 */
        rc = pager_unpin(pg, page_ids[i], false);
        if (rc != OK) {
            ovf_free_page_list(pg, page_ids, i + 1u);
            return rc;
        }
    }

    /*
     * ============ 第二阶段：逐页写"最终内容"，每页只改一次 ============
     *
     * 顺序铁律（loghook.h）：**先写日志，再改页**。
     * 所以每页先算出完整的内容快照，用它当 after；before 是全 0
     * （pager_alloc 已经把整页清零，含复用出来的空闲页 —— 见 pager.c）。
     */
    for (i = 0; i < npages; i++) {
        PageFrame *fr = NULL;
        u8 before[PAGE_SIZE];
        u8 after[PAGE_SIZE];
        u32 chunk = len - off;
        u32 lsn = 0;

        if (chunk > JSON_OVF_PAY_CAP)
            chunk = JSON_OVF_PAY_CAP;

        rc = pager_get(pg, page_ids[i], &fr);
        if (rc != OK)
            goto fail;

        /*
         * ============ before 镜像必须是"空闲页"，不是全 0 ============
         *
         * 这一页的**逻辑前状态**是"无主的空闲页"：
         *   - 若是从空闲栈复用来的，它原本就是 PT_FREE；
         *   - 若是文件尾新扩的，它从没被写过。
         *
         * 两种情况下 undo 都该让它回到"可回收"的形态。
         *
         * 【为什么不能用全 0 当 before】
         * undo 把全 0 盖回去 ⇒ 页头 H_TYPE = 0 ⇒ 既不是 PT_JSON_OVF
         * 也不是 PT_FREE ⇒ `pager_free_rebuild` 扫页时**两头都认不出它**
         * ⇒ 这个页号永久泄漏（既不在 freelist，也无人引用）。
         *
         * 用 PT_FREE 当 before，undo 后页回到空闲态 ⇒ 开库重建空闲栈时
         * 能被扫出来并回收 ⇒ 未提交事务留下的链页不会泄漏。
         */
        memcpy(before, zeros, PAGE_SIZE);
        (void)page_init(before, (u8)PT_FREE);

        /* 在**本地快照**里把最终内容拼好，不碰页 */
        memcpy(after, zeros, PAGE_SIZE);
        (void)page_init(after, (u8)PT_JSON_OVF);
        if (chunk)
            memcpy(after + JSON_OVF_DATA_OFF, data + off, chunk);
        put_u32(after + JSON_OVF_LEN_OFF, chunk);
        put_u32(after + JSON_OVF_NEXT_OFF,
                (i + 1u < npages) ? page_ids[i + 1] : PAGE_ID_NIL);
        page_set_checksum(after);

        if (wal != NULL && wal->fn != NULL) {
            rc = wal->fn(wal->ctx, wal->xid, (u8)LOG_PAGEDATA,
                         page_ids[i], PAGE_SLOT_NONE, LOG_NO_SLOT_OFF,
                         before, (u16)PAGE_SIZE,
                         after, (u16)PAGE_SIZE, &lsn);
            if (rc != OK) {
                (void)pager_unpin(pg, page_ids[i], false);
                goto fail;
            }
        }

        /* 日志已经落定，这时才改页 */
        memcpy(fr->data, after, PAGE_SIZE);
        if (lsn)
            page_set_lsn(fr->data, lsn);

        rc = pager_unpin(pg, page_ids[i], true);
        if (rc != OK)
            goto fail;

        off += chunk;
    }

    *ext_key_out = (u64)page_ids[0];
    return OK;

fail:
    /*
     * 补偿：整条链都是本次新建的，没有 tuple 引用它 ⇒ 全部归还。
     *
     * ⚠️ 用逐页归还而**不是** json_ovf_free：
     *    失败时链的后半段还没写内容（页头是全 0），按链走会在半路
     *    判成"链损坏"并中止 ⇒ 后半段页泄漏。
     */
    ovf_free_page_list(pg, page_ids, npages);
    return rc;
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

/* ---------------- 释放 ---------------- */

/*
 * 释放一条页链：把每一页清零，再还给全局页分配器。
 *
 * ⚠️ 先清零的理由见坑 M7-29（下一个主人不该看到上一个主人的数据）。
 *
 * ⚠️ 释放之前调用方必须确保**没有人在读这条链**。
 */
int json_ovf_free(Pager *pg, u64 ext_key)
{
    u32 ids[JSON_OVF_MAX_PAGES];
    u32 pid;
    u32 pages = 0;
    u32 i;
    int rc;

    if (!pg)
        return E_INVAL;
    if (ext_key > 0xFFFFFFFFu)
        return E_INVAL;

    /*
     * ============ 幂等契约 ============
     *
     * VACUUM 是长事务，中途崩溃后再跑一次就会重复调用本函数。
     * 所以这里**不靠"不会重复调用"的假设**，而是靠页头自证：
     *
     *   - 首页已经不是 PT_JSON_OVF ⇒ 这条链早被释放过（或已被别人接管）
     *     ⇒ 直接返回 OK（空操作）；
     *   - 首页是 PT_JSON_OVF、但中间某页不是 ⇒ 链真的坏了
     *     ⇒ 返回 E_CORRUPT，且**一个页都不释放**。
     *
     * 第二种是"链被改坏"，不是"已释放"，两者必须区分开 ——
     * 把损坏的链当已释放就会静默丢数据。
     */
    pid = (u32)ext_key;
    while (pid != PAGE_ID_NIL) {
        PageFrame *fr = NULL;

        /* 闸门：链比这还长 ⇒ 成环了 */
        if (pages >= JSON_OVF_MAX_PAGES)
            return E_CORRUPT;

        rc = pager_get(pg, pid, &fr);
        if (rc != OK) {
            if (pages == 0)
                return OK;      /* 首页都取不到 ⇒ 早就不存在了 */
            return E_CORRUPT;
        }

        if (fr->data[H_TYPE] != (u8)PT_JSON_OVF ||
            !page_verify_checksum(fr->data)) {
            (void)pager_unpin(pg, pid, false);
            return (pages == 0) ? OK : E_CORRUPT;
        }

        ids[pages] = pid;
        pid = get_u32(fr->data + JSON_OVF_NEXT_OFF);
        pages++;
        (void)pager_unpin(pg, ids[pages - 1], false);
    }

    /*
     * 先"拆链"再"还页"：把首页标记清掉，后续任何重复调用都会走
     * "首页不是 PT_JSON_OVF ⇒ OK" 那条快捷路径。
     *
     * ⚠️ 顺序很重要：如果先还页、再清标记，中途崩溃会留下
     *    "页已进 freelist、页头还是 PT_JSON_OVF" 的状态 ⇒
     *    重跑时会再还一次 ⇒ double-free。
     */
    for (i = 0; i < pages; i++) {
        PageFrame *fr = NULL;

        rc = pager_get(pg, ids[i], &fr);
        if (rc != OK)
            return rc;

        /* 清零：让下一个主人看到的是空页，而不是上一个主人的文档 */
        memset(fr->data, 0, PAGE_SIZE);

        rc = pager_unpin(pg, ids[i], true);
        if (rc != OK)
            return rc;

        rc = pager_free(pg, ids[i]);
        if (rc != OK)
            return rc;
    }

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
    (void)off;

    if (!ctx)
        return E_INVAL;

    return json_ovf_read((Pager *)ctx, key, buf, cap, len_out);
}
