/*
 * ft_tomb.c —— tombstone（删除标记）集合的**落盘**与恢复（v1.3 步骤 5）
 *
 * 【这一层管什么】
 *
 * "这篇文档被删了"这件事必须存在**两个地方**：
 *
 *   1. 内存里的 ix->tomb（升序的 doc_id 集合）—— 检索时用来过滤；
 *   2. 磁盘上的 tombstone 页（PT_FT_TOMB）—— **权威存储**。
 *
 * 为什么第 2 份是必需的（坑 B）：
 *
 * 只靠 WAL 记录是不够的。WAL 会被 checkpoint **截断** ——
 * 截断之后老的删除记录就没了，而那些文档还在段里，
 * 于是它们"复活"了：删掉的东西又出现在检索结果里。
 *
 * 所以 tombstone 集合必须自己落盘（WAL 只负责"崩溃时怎么把它找回来"）。
 *
 * 【落盘形态】
 *
 * 一页就是一个稠密的 u32 数组（格式见 ft_internal.h 的 FTM_*）：
 *
 *   20 FTM_COUNT  u32   本页的 doc_id 条数
 *   24 FTM_DATA   ...   doc_id 数组
 *
 * 写满就**再开一页**（每个 tombstone 页都是独立的，
 * 全体的并集 = 集合）。这样就不需要链头，也就不怕"链断在半路"。
 *
 * 【每次删除都要写整页吗 —— 不写了（v1.3 欠账 1 · 坑 M4-24）】
 *
 * 原来每次删除把当前页**读-改-写整页**，WAL 里记整页镜像（LOG_PAGEDATA）。
 * 好处是"恢复只要把页盖回去"；坏处是**undo 也是整页盖回去**：
 *
 *   同一页上，T2（未提交）先追加了 B，T1（已提交）后追加了 A，
 *   那么 T2 那条记录的整页前像 = [B]（**不含** A）。
 *   崩溃恢复撤销 T2 时整页盖回 [B] → **A 被连带抹掉** →
 *   已提交的删除"复活"（那篇文档又能检索到了）。
 *
 * 坑 M4-26：undo 不能再用整页前像 —— 整页前像只是"某一时刻的快照"，
 * 撤销其中一条记录等于把"那之后的**所有**改动"一起撤销掉。
 *
 * 【现在的做法：一条逻辑记录 = 页内的一条】
 *
 * 每次删除写一条**逻辑 WAL 记录** WAL_FT_TOMB_APPEND：
 *
 *   载荷 = u32 page_id + u32 index + u32 doc_id（12 字节）
 *
 *   - 重放（redo）：把 doc_id 追加到那一页（**幂等**：页里已经有了就跳过）；
 *   - 撤销（undo）：把**那一条**从那一页里移除（只动自己那一条，
 *     同页其它已提交的删除毫发无损）。
 *
 * tombstone 页因此**不再走整页镜像 WAL**；整页镜像只用于"新建一张空页"，
 * 而且用**系统事务 xid=0** 记 —— 一张空页的存在本身无害，
 * 不能因为一个未提交事务的回滚把整页抹成 0（那会连带抹掉同页其它删除）。
 *
 * 【为什么撤销要"按值"删，不能"按位置"删】
 *
 * 同一页上，已提交的追加可能排在未提交的追加**后面**。
 * 撤销时把中间那条抽掉，会让后面的条目**移位** ——
 * 所以撤销必须按 **doc_id** 找、再把后面的条目往前挪，
 * 不能按"记录里写的那个 index"直接覆盖。
 * （集合本身是**集合**：顺序无所谓，移位不影响语义。）
 *
 * 坑 M4-27：重放阶段必须跳过"已经回滚完的事务"（日志里有 WAL_ABORT_DONE）——
 * 否则第二次恢复会把上一轮已经撤销掉的删除又追加回来。
 */

#include "modalities/ft/ft_index_internal.h"
#include "modalities/ft/ft_tomb.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "modalities/ft/ft_internal.h"
#include "modalities/ft/ft_writer.h"
#include "storage/page.h"

/* ---------------- 小工具 ---------------- */

/* u32 升序比较（qsort 用） */
static int ft_tomb_u32_cmp(const void *pa, const void *pb)
{
    u32 a = *(const u32 *)pa;
    u32 b = *(const u32 *)pb;

    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

/* ---------------- 内存里的集合（升序数组） ---------------- */

/*
 * 二分找 doc_id 在升序集合里的位置（返回第一个 >= doc_id 的下标，也就是插入点）。
 *
 * ft_merge.h 要求传过去的 tomb 集合**必须升序**（那边用二分判断成员）。
 */
u32 ft_tomb_lb(const u32 *tomb, u32 n, u32 doc_id)
{
    u32 lo = 0;
    u32 hi = n;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;

        if (tomb[mid] < doc_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* doc_id 是否已被删除 */
bool ft_tomb_is(const FtIndex *ix, u32 doc_id)
{
    u32 pos = ft_tomb_lb(ix->tomb, ix->n_tombs, doc_id);

    return (pos < ix->n_tombs && ix->tomb[pos] == doc_id);
}

/*
 * 升序插入一个 doc_id。
 *
 * 返回 1 = 插进去了，0 = 已经在集合里（空操作），-1 = 内存不足。
 *
 * 【为什么是"有序插入"而不是"追加后整体排序"】
 *
 *   - 有序插入：插入 O(n)（memmove），但**查找**是 O(log n) 二分；
 *     追加后排序：插入 O(n log n)，查找也只能 O(n) 线性扫。
 *   - 删除是**低频**操作，检索是**高频**操作 —— 该优化的是检索。
 *   - ft_merge.h 要求传过去的 tomb 集合必须升序（那边用二分判断成员），
 *     有序插入天然满足。
 */
int ft_tomb_insert(FtIndex *ix, u32 doc_id)
{
    u32 pos = ft_tomb_lb(ix->tomb, ix->n_tombs, doc_id);

    if (pos < ix->n_tombs && ix->tomb[pos] == doc_id)
        return 0;               /* 已经删过了：再删一次是合法的空操作 */

    if (ix->n_tombs == ix->cap_tombs) {
        u32 ncap = (ix->cap_tombs == 0) ? 16u : ix->cap_tombs * 2u;
        u32 *nt = (u32 *)realloc(ix->tomb, sizeof(u32) * (size_t)ncap);

        if (nt == NULL)
            return -1;
        ix->tomb = nt;
        ix->cap_tombs = ncap;
    }
    if (pos < ix->n_tombs) {
        memmove(&ix->tomb[pos + 1], &ix->tomb[pos],
                sizeof(u32) * (size_t)(ix->n_tombs - pos));
    }
    ix->tomb[pos] = doc_id;
    ix->n_tombs++;
    return 1;
}

/* ---------------- 写日志的回调（进程级一份） ---------------- */

/*
 * 写 tombstone 追加记录的回调（见 ft_tomb.h）。
 *
 * 坑 M4-28：这里只注册**函数**，不注册上下文 ——
 * 上下文由调用方（ft_tomb.c）每次传 ix->wal_ctx 下来，
 * 同时开几个库时不会互相串。
 */
static FtTombLogFn g_tomb_log = NULL;

void ft_tomb_set_log_hook(FtTombLogFn fn)
{
    g_tomb_log = fn;
}

/* ---------------- 页的登记与追加 ---------------- */

/*
 * 登记一张 tombstone 页（扫描页空间时调）。
 *
 * count —— 这一页里有多少条（调用方已经读过了）。
 *
 * 【"当前页"是谁】
 *
 * 约定：**最后登记的那张页**就是当前页（tomb_fill = 它的条数）。
 * 追加时先往它后面写，写满了就再开一页（新页自然成了最后一张）。
 *
 * 扫描是按页号递增做的，所以最后一张 = 页号最大的那张。
 */
int ft_tomb_add_page(FtIndex *ix, u32 page_id, u32 count)
{
    if (ix == NULL)
        return E_INVAL;

    if (ix->n_tomb_pages == ix->cap_tomb_pages) {
        u32 ncap = (ix->cap_tomb_pages == 0) ? 4u : ix->cap_tomb_pages * 2u;
        u32 *np = (u32 *)realloc(ix->tomb_pages, sizeof(u32) * (size_t)ncap);

        if (np == NULL)
            return E_NOSPACE;
        ix->tomb_pages = np;
        ix->cap_tomb_pages = ncap;
    }
    ix->tomb_pages[ix->n_tomb_pages++] = page_id;
    ix->tomb_fill = count;
    return OK;
}

/*
 * 追加一个 doc_id 到落盘的集合。
 *
 * ==================== 现在的写法（v1.3 欠账 1 · 坑 M4-24） ====================
 *
 * 页里没有"插入"这个操作 —— 它就是一个数组，
 * 追加 = 把 id 写到第 count 个位置，count 加一。
 *
 * 【写日志的时机：先日志，后改页】
 *
 *   1. 写一条逻辑记录 WAL_FT_TOMB_APPEND（载荷 = u32 doc_id，
 *      记录头里的 page_id = 这一页）→ 拿到它的 LSN；
 *   2. 再改页（把 doc_id 写进去），并把页的 LSN 设成这条记录的 LSN。
 *
 * 反过来的话，页可能先于日志落盘 → 崩溃后日志里缺这条记录，
 * 页上却已经有它的内容 → 删除"没根"。
 *
 * 【为什么新页还要整页镜像】
 *
 * 只有"新建一张空页"这一次用整页镜像，而且记在**系统事务 xid=0** 下：
 * 一张空页的存在本身无害，不能因为某个未提交事务的回滚
 * 把整页抹成 0（那会连带抹掉同页上别的已提交删除 —— 坑 M4-26 同款问题）。
 */
int ft_tomb_append(FtIndex *ix, u32 xid, u32 doc_id)
{
    u8 *buf = NULL;
    PageFrame *fr = NULL;
    u32 pid = PAGE_ID_NIL;
    u32 count = 0;
    u32 lsn = 0;
    int rc;

    if (ix == NULL)
        return E_INVAL;

    if (ix->n_tomb_pages > 0 && ix->tomb_fill < FT_TOMB_CAP) {
        /* 当前页还没满 → 接着它写 */
        pid = ix->tomb_pages[ix->n_tomb_pages - 1];
        rc = pager_get(ix->pg, pid, &fr);
        if (rc != OK)
            return rc;
        count = get_u32(fr->data + FTM_COUNT);
        if (count >= FT_TOMB_CAP) {
            /* 页里的 count 与内存里记的对不上（不该发生）→ 当它满了，另开一页 */
            pager_unpin(ix->pg, pid, false);
            fr = NULL;
            pid = PAGE_ID_NIL;
        }
    }

    if (fr == NULL) {
        /* 开一张新页：整页镜像进 WAL（系统事务 → 永远不会被撤销） */
        rc = pager_alloc(ix->pg, &pid, &fr);
        if (rc != OK)
            return rc;

        buf = (u8 *)malloc(PAGE_SIZE);
        if (buf == NULL) {
            pager_unpin(ix->pg, pid, false);
            return E_NOSPACE;
        }
        memset(buf, 0, PAGE_SIZE);
        buf[H_TYPE] = (u8)PT_FT_TOMB;
        put_u32(buf + FTM_COUNT, 0);

        rc = ft_page_write(ix->pg, ix->wal, ix->wal_ctx, 0u, pid, fr, buf, NULL);
        free(buf);
        buf = NULL;
        if (rc != OK)
            return rc;        /* ft_page_write 已经放开了这一帧 */

        /* ft_page_write 一定会放开帧 → 重新取一次再改 */
        rc = pager_get(ix->pg, pid, &fr);
        if (rc != OK)
            return rc;
        count = 0;

        /*
         * 【先把页号登记进段目录，再往页里写删除】
         *
         * 目录里列着页号，页里的内容才有意义。
         *
         * 顺序反过来的话：页里已经写进了删除、目录里却还没有这个页号 →
         * 断电后恢复时按目录找页，找不到 → **那次删除凭空消失**
         * （被删的文档又冒出来了）。
         *
         * 现在的顺序：目录先写（页号列上了，页是空的）→ 再写删除。
         * 断电停在中间 → 目录里有一张**空的** tombstone 页 →
         * 没有删除丢失 ✓（那一次删除还没返回成功，丢掉是对的）。
         */
        rc = ft_tomb_add_page(ix, pid, 0);
        if (rc != OK) {
            pager_unpin(ix->pg, pid, false);
            return rc;
        }
        /*
         * 【目录这一写必须用系统事务 xid = 0】（坑 M4-49）
         *
         * 这一页是**新分配的页**，它的存在本身与事务无关
         * （页的整页镜像本来就是记在 xid=0 下的）。
         * 所以"把页号登记进目录"也必须是 xid=0 ——
         * **不能**跟着删文档的那个事务走。
         *
         * 反过来会怎样（这是实测撞出来的 bug）：
         *   事务 T1 未提交地删了 B → 新建 tombstone 页 P，目录登记 P（记在 T1 下）
         *   事务 T2 提交地删了 A → A 也追加进 P
         *   T1 回滚 → 目录这一写被撤销 → P 从目录里消失
         *            → **T2 已提交的删除 A 跟着一起丢了**（文档复活）
         *
         * 也就是说：一次**没提交**的删除，把别人**已提交**的删除一起抹掉了 ——
         * 与坑 M4-26（整页前像连带抹掉同页已提交的删除）是**同一款病**，
         * 只是这次病在"目录"上，不在"页"上。
         *
         * 改成 xid=0 之后：
         *   - 目录里列着 P（P 是空页，或只有别的已提交删除）；
         *   - T1 回滚只撤销"页里那一条"（WAL_FT_TOMB_APPEND 的 undo），
         *     同页上别的已提交删除毫发无损 ✓；
         *   - 代价：一次回滚的删除会留下一张**空页**在目录里（浪费一页，可接受）。
         *
         * 【为什么不会和合并打架】
         *
         * 目录只有一处写入口 ft_catalog_sync()，而它的 xid 有两个来源：
         *   - 加文档 / 合并：**本来就是 xid = 0**（db_ft.c 里传的就是 0，
         *     它们是系统操作，不参与用户事务的提交/回滚）；
         *   - 删文档：本文件这一处（现在也是 0）。
         * 所以目录**永远不会被撤销** ——
         * 段列表的变化（加段、合并标死老段）也就不需要靠目录回滚来撤销。
         */
        rc = ft_catalog_sync(ix, 0u);
        if (rc != OK) {
            /* 目录写不进去 → 撤掉登记（这一页就当没建过） */
            ix->n_tomb_pages--;
            pager_unpin(ix->pg, pid, false);
            return rc;
        }
    }

    /* 先写日志（拿到 LSN），再改页 */
    if (g_tomb_log) {
        rc = g_tomb_log(ix->wal_ctx, xid, pid, doc_id, &lsn);
        if (rc != OK) {
            /* 日志没写好 → 这一页不许改 */
            pager_unpin(ix->pg, pid, false);
            return rc;
        }
    }

    /* 追加这一条 */
    put_u32(fr->data + FTM_DATA + count * 4u, doc_id);
    put_u32(fr->data + FTM_COUNT, count + 1u);
    if (lsn != 0)
        page_set_lsn(fr->data, lsn);
    page_set_checksum(fr->data);

    rc = pager_unpin(ix->pg, pid, true);
    if (rc != OK)
        return rc;

    /*
     * "当前页"的条数：新页刚才是登记过（count=0）的，
     * 老页本来就是接着往后写 —— 两种情况都是 count + 1。
     */
    ix->tomb_fill = count + 1u;
    return OK;
}

/* ---------------- 恢复期：改 tombstone 页 ---------------- */

/*
 * 页里按值找 doc_id（找不到返回 FT_TOMB_NOT_FOUND）。
 */
#define FT_TOMB_NOT_FOUND 0xffffffffu

static u32 ft_tomb_page_find(const u8 *data, u32 count, u32 doc_id)
{
    for (u32 i = 0; i < count; i++) {
        if (get_u32(data + FTM_DATA + i * 4u) == doc_id)
            return i;
    }
    return FT_TOMB_NOT_FOUND;
}

/*
 * 取 tombstone 页；页不在文件里就现场开一张空的。
 *
 * 【什么时候会碰到"页不在"】
 *
 * 正常不会：建页的整页镜像（系统事务）的 LSN 一定比追加记录小，
 * 恢复时先重放建页、再重放追加 ✓。
 *
 * 但"建页记录被 checkpoint 跳过、而页又没落盘"这种边角情况
 * （页池复用的页刚被分配、还没刷过盘）可能出现 ——
 * 这时宁可现场开一张空的，也不能让整个开库失败。
 */
static int ft_tomb_page_get(Pager *pg, u32 page_id, PageFrame **fr_out, u32 *count_out)
{
    PageFrame *fr = NULL;
    u32 count;
    int rc;

    rc = pager_get(pg, page_id, &fr);
    if (rc == E_NOTFOUND) {
        rc = pager_extend_to(pg, page_id + 1u);
        if (rc != OK)
            return rc;
        rc = pager_get(pg, page_id, &fr);
    }
    if (rc != OK)
        return rc;

    if (fr->data[H_TYPE] != (u8)PT_FT_TOMB) {
        /* 不是 tombstone 页：当成一张空的来（内容会被下面的写入覆盖） */
        memset(fr->data, 0, PAGE_SIZE);
        fr->data[H_TYPE] = (u8)PT_FT_TOMB;
        put_u32(fr->data + FTM_COUNT, 0);
    }

    count = get_u32(fr->data + FTM_COUNT);
    if (count > FT_TOMB_CAP)
        count = FT_TOMB_CAP;         /* 页坏了也不能越界读 */

    *fr_out = fr;
    *count_out = count;
    return OK;
}

/*
 * 重放：把 doc_id 追加到那一页（幂等）。
 *
 * 【幂等靠两件事】
 *
 *   1. 页 LSN：页 LSN >= 这条记录的 LSN → 说明它已经生效过 → 跳过；
 *   2. 按值判重：页里已经有这个 doc_id → 跳过（不追加第二份）。
 *
 * 第 2 条是兜底：万一页 LSN 因为别的原因领先了，
 * 也不能把同一条删除追加成两份（那会让 tombstone 数虚高）。
 */
int ft_tomb_page_append(Pager *pg, u32 page_id, u32 doc_id, u32 lsn)
{
    PageFrame *fr = NULL;
    u32 count = 0;
    int rc;

    if (pg == NULL)
        return E_INVAL;

    rc = ft_tomb_page_get(pg, page_id, &fr, &count);
    if (rc != OK)
        return rc;

    /* 已经生效过 → 什么都不做 */
    if (lsn != 0 && page_lsn(fr->data) >= lsn) {
        pager_unpin(pg, page_id, false);
        return OK;
    }

    /* 已经有这一条了 → 不追加第二份 */
    if (ft_tomb_page_find(fr->data, count, doc_id) != FT_TOMB_NOT_FOUND) {
        if (lsn != 0 && page_lsn(fr->data) < lsn)
            page_set_lsn(fr->data, lsn);
        page_set_checksum(fr->data);
        pager_unpin(pg, page_id, true);
        return OK;
    }

    if (count >= FT_TOMB_CAP) {
        /* 页满了：这一条放不下（正常不会发生 —— 写路径会先开新页） */
        pager_unpin(pg, page_id, false);
        return E_NOSPACE;
    }

    put_u32(fr->data + FTM_DATA + count * 4u, doc_id);
    put_u32(fr->data + FTM_COUNT, count + 1u);
    if (lsn != 0)
        page_set_lsn(fr->data, lsn);
    page_set_checksum(fr->data);

    return pager_unpin(pg, page_id, true);
}

/*
 * 回滚：把**那一条**从那一页里移除（按值找）。
 *
 * 【只动自己那一条】
 *
 * 找得到 → 把它后面的条目往前挪一格、count 减一；
 * 找不到 → 什么都不做（已经撤过了 / 从没生效过）。
 *
 * 同页上别的已提交删除**毫发无损** —— 这正是坑 M4-24 要修的东西。
 */
int ft_tomb_page_remove(Pager *pg, u32 page_id, u32 doc_id, u32 lsn)
{
    PageFrame *fr = NULL;
    u32 count = 0;
    u32 pos;
    int rc;

    if (pg == NULL)
        return E_INVAL;

    rc = ft_tomb_page_get(pg, page_id, &fr, &count);
    if (rc != OK) {
        /* 页都不在了 → 没什么可撤的 */
        return (rc == E_NOTFOUND) ? OK : rc;
    }

    pos = ft_tomb_page_find(fr->data, count, doc_id);
    if (pos == FT_TOMB_NOT_FOUND) {
        /* 这一条不在页里：已经撤过了 */
        if (lsn != 0 && page_lsn(fr->data) < lsn) {
            /*
             * 抬页 LSN：告诉"下一次恢复"这条记录已经处理过了，
             * 别再把它追加回来。
             */
            page_set_lsn(fr->data, lsn);
            page_set_checksum(fr->data);
            pager_unpin(pg, page_id, true);
            return OK;
        }
        pager_unpin(pg, page_id, false);
        return OK;
    }

    /* 把它后面的条目往前挪一格 */
    for (u32 k = pos; k + 1u < count; k++) {
        put_u32(fr->data + FTM_DATA + k * 4u,
                get_u32(fr->data + FTM_DATA + (k + 1u) * 4u));
    }
    put_u32(fr->data + FTM_COUNT, count - 1u);
    if (lsn != 0 && page_lsn(fr->data) < lsn)
        page_set_lsn(fr->data, lsn);
    page_set_checksum(fr->data);

    return pager_unpin(pg, page_id, true);
}

/* ---------------- 恢复：装配集合 ---------------- */

/*
 * 从 tombstone 页里读出全部 doc_id（追加到 arr，返回写到的位置）。
 *
 * 【读不出来 / 不是 tombstone 页 → 当成空的，跳过】
 *
 * 段目录里可能列着一张**还没写内容**的 tombstone 页：
 * 新建页的顺序是"先写目录（页号列上）→ 再往页里写删除"，
 * 断电停在中间就会这样。那一页里一条删除都没有 —— 跳过它是对的。
 *
 * 早先这里是 E_CORRUPT（读不出来就整个开库失败）——
 * 那太脆了：一次没提交的删除会把整个库卡住。
 */
static int ft_tomb_read_page(FtIndex *ix, u32 pid, u32 *arr, u32 cap, u32 *pos)
{
    PageFrame *fr = NULL;
    u32 count;
    int rc;

    rc = pager_get(ix->pg, pid, &fr);
    if (rc != OK)
        return OK;            /* 页不在 → 当成空的 */

    if (fr->data[H_TYPE] != (u8)PT_FT_TOMB) {
        pager_unpin(ix->pg, pid, false);
        return OK;            /* 不是 tombstone 页 → 当成空的 */
    }
    count = get_u32(fr->data + FTM_COUNT);
    if (count > FT_TOMB_CAP)
        count = FT_TOMB_CAP;         /* 页坏了也不能越界读 */

    if (*pos + count > cap) {
        pager_unpin(ix->pg, pid, false);
        return E_NOSPACE;
    }
    for (u32 i = 0; i < count; i++)
        arr[(*pos)++] = get_u32(fr->data + FTM_DATA + i * 4u);

    pager_unpin(ix->pg, pid, false);
    return OK;
}

/*
 * 装配最终的 tombstone 集合。
 *
 *   最终集合 = 页里的内容 ∪ (WAL 重放看到的 − WAL 回滚撤销的)
 *
 * 【为什么是"并集"而不是"取其一"】
 *
 * 页里的内容是权威的（不会被 checkpoint 截断），
 * WAL 那一路补的是"页还没落盘、日志里已经记了"的删除。
 * 两者都要 —— 只取页会丢掉最后几笔删除，只取 WAL 会在日志被截断后丢历史。
 *
 * 【为什么还要减掉"撤销"的那一份】
 *
 * 未提交事务的删除 = "这个删除没发生过"。
 * 页里的那份会被 undo 的前像恢复掉，
 * 但 WAL 那一路仍然会看到这条删除记录 —— 不减掉它，
 * 未提交事务的删除就"诈尸"了（页面恢复了，内存集合却还删着）。
 *
 * 【doc_id 在集合里被删过几次，只算一次】
 *
 * 集合是**集合**：排序后去重。
 */
int ft_tomb_assemble(FtIndex *ix, const u32 *redo_ids, u32 n_redo,
                      const u32 *undo_ids, u32 n_undo)
{
    u32 *arr = NULL;
    u32 n = 0;
    u32 cap;
    u32 total = 0;
    int rc;

    if (ix == NULL)
        return E_INVAL;

    /* 页里一共多少条 */
    for (u32 i = 0; i < ix->n_tomb_pages; i++) {
        PageFrame *fr = NULL;

        if (pager_get(ix->pg, ix->tomb_pages[i], &fr) != OK)
            continue;
        if (fr->data[H_TYPE] == (u8)PT_FT_TOMB) {
            u32 c = get_u32(fr->data + FTM_COUNT);

            if (c <= FT_TOMB_CAP)
                total += c;
        }
        pager_unpin(ix->pg, ix->tomb_pages[i], false);
    }

    cap = total + n_redo + 1u;
    arr = (u32 *)malloc(sizeof(u32) * (size_t)cap);
    if (arr == NULL)
        return E_NOSPACE;

    /* 页里的内容 */
    for (u32 i = 0; i < ix->n_tomb_pages; i++) {
        rc = ft_tomb_read_page(ix, ix->tomb_pages[i], arr, cap, &n);
        if (rc != OK) {
            free(arr);
            return rc;
        }
    }

    /* WAL 重放看到的（未被回滚撤销的） */
    for (u32 i = 0; i < n_redo; i++) {
        bool undone = false;

        for (u32 k = 0; k < n_undo; k++) {
            if (undo_ids[k] == redo_ids[i]) {
                undone = true;
                break;
            }
        }
        if (undone)
            continue;
        if (n >= cap) {
            free(arr);
            return E_NOSPACE;
        }
        arr[n++] = redo_ids[i];
    }

    /* 排序 + 去重 */
    if (n > 1)
        qsort(arr, n, sizeof(u32), ft_tomb_u32_cmp);
    {
        u32 k = 0;

        for (u32 i = 0; i < n; i++) {
            if (k > 0 && arr[k - 1] == arr[i])
                continue;
            arr[k++] = arr[i];
        }
        n = k;
    }

    /* 装进索引的内存集合 */
    free(ix->tomb);
    ix->tomb = arr;
    ix->n_tombs = n;
    ix->cap_tombs = cap;
    return OK;
}

void ft_tomb_free(FtIndex *ix)
{
    if (ix == NULL)
        return;
    free(ix->tomb_pages);
    ix->tomb_pages = NULL;
    ix->n_tomb_pages = 0;
    ix->cap_tomb_pages = 0;
    ix->tomb_fill = 0;
}
