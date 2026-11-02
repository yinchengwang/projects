/*
 * blob.c — blob 模态的实现
 *
 * 设计说明见 blob.h。这里只讲实现上的关键点。
 */

#include "storage/blob.h"
#include "storage/blob_hdr.h"

#include <stdlib.h>
#include <string.h>

#include "common/crc.h"
#include "storage/page.h"

struct BlobStore {
    Pager    *pager;
    LogHookFn wal;
    void     *wal_ctx;
    BlobStats stats;

    /*
     * 对象目录（内存）。
     *
     * 【为什么 blob 有自己的目录，而不用 catalog】
     *
     * catalog 是**关系模态**的元数据。blob 依赖它 = 模态之间互相依赖，
     * 这正是 SPI 明确禁止的。所以 blob 自己记一份。
     *
     * 【重启后怎么重建】
     *
     * 扫一遍所有页，找 PT_BLOB 且带"头页标志"的页 → 就是对象头页。
     * blob_id = 页号 + 1。
     *
     * 这比维护一个持久化的目录页简单得多，而且不会有一致性问题
     * （目录页和实际对象对不上）。代价是启动时扫一遍全文件 ——
     * 和关系模态重建 freelist 是同一个代价量级。
     */
    BlobId  *ids;
    u32      n_ids;
    u32      cap_ids;
};

/*
 * blob 头页标志（H_FLAGS 的 bit3）。
 *
 * 用来区分「blob 头页」和「blob 帧页」——
 * 两者页类型都是 PT_BLOB，但只有头页带这个标志。
 */
#define BLOB_FLAG_HEAD  0x08u

/* ---------------- WAL ---------------- */

/*
 * 写一条整页镜像日志。
 *
 * 用 LOG_PAGEDATA（before/after 各 4KB）。
 *
 * 为什么 before 也要给：undo 阶段要能把这页还原回去。
 * blob 页是**新分配**的，before 其实是全 0 —— 但我们仍然把当前页内容
 * 拍下来当 before，这样万一以后 blob 页支持复用（页从空闲池里拿），
 * undo 语义自动是对的。
 *
 * WAL 规则：先写日志，再改页。所以调用顺序是
 *   1. 算出这次修改后的页内容（after）
 *   2. 写日志，拿到 LSN
 *   3. 把 after 落到页里，把 LSN 写进页头
 */
static int blob_log_page(BlobStore *bs, u32 xid, u32 page_id,
                         const u8 *before, const u8 *after, u32 *lsn_out)
{
    *lsn_out = 0;
    if (!bs->wal)
        return OK;

    /*
     * WAL_MAX_PAYLOAD 是 8192，before(4096) + after(4096) 刚好装下。
     * 这是 WAL_PAGEDATA 的固有代价 —— 日志膨胀一倍。
     * blob 对象不可变、每页只写一次，所以这个代价是固定的、可接受的。
     */
    return bs->wal(bs->wal_ctx, xid, LOG_PAGEDATA, page_id,
                   PAGE_SLOT_NONE, LOG_NO_SLOT_OFF,
                   before, (u16)PAGE_SIZE, after, (u16)PAGE_SIZE,
                   lsn_out);
}

/* ---------------- id ↔ 页号 ---------------- */

/*
 * blob_id → 头页页号。
 *
 * blob_id = 页号 + 1（见 blob.h 里为什么要 +1）。
 * 这里集中一处转换，避免到处写 id - 1 出错。
 */
static u32 blob_head_page(BlobId id)
{
    return (u32)(id - 1);
}

/* ---------------- 对象目录 ---------------- */

static int blob_dir_push(BlobStore *bs, BlobId id)
{
    if (bs->n_ids >= bs->cap_ids) {
        u32 ncap = bs->cap_ids ? bs->cap_ids * 2 : 16;
        BlobId *nid = realloc(bs->ids, (size_t)ncap * sizeof(BlobId));
        if (!nid)
            return E_IO;
        bs->ids = nid;
        bs->cap_ids = ncap;
    }
    bs->ids[bs->n_ids++] = id;
    return OK;
}

/*
 * 扫全文件重建目录。
 *
 * 找 PT_BLOB 且带「头页标志」的页 → 那是对象头页。
 * blob_id = 页号 + 1。
 */
static void blob_dir_scan(BlobStore *bs)
{
    u64 np = pager_npages(bs->pager);

    for (u64 p = 0; p < np; p++) {
        PageFrame *f;
        if (pager_get(bs->pager, (u32)p, &f) != OK)
            continue;

        bool is_head = (f->data[H_TYPE] == PT_BLOB) &&
                       ((f->data[H_FLAGS] & BLOB_FLAG_HEAD) != 0);
        pager_unpin(bs->pager, (u32)p, false);

        if (is_head)
            (void)blob_dir_push(bs, (BlobId)p + 1);
    }
}

u32 blob_count(const BlobStore *bs)
{
    return bs ? bs->n_ids : 0;
}

BlobId blob_id_at(const BlobStore *bs, u32 i)
{
    if (!bs || i >= bs->n_ids)
        return BLOB_ID_INVALID;
    return bs->ids[i];
}

/* ---------------- 生命周期 ---------------- */

int blob_store_create(Pager *pg, LogHookFn fn, void *ctx, BlobStore **out)
{
    if (!pg || !out)
        return E_INVAL;

    BlobStore *bs = calloc(1, sizeof(*bs));
    if (!bs)
        return E_IO;
    bs->pager = pg;
    bs->wal = fn;
    bs->wal_ctx = ctx;

    /*
     * 重建对象目录 —— 打开数据库时，之前创建的对象要能被找到。
     *
     * 这和关系模态"恢复后重建 freelist"是同一个代价量级：
     * 扫一遍全文件。选它而不是维护持久化目录页，
     * 是因为**不会有一致性问题**（目录页和实际对象可能对不上）。
     */
    blob_dir_scan(bs);

    *out = bs;
    return OK;
}

void blob_store_destroy(BlobStore *bs)
{
    if (!bs)
        return;
    free(bs->ids);
    free(bs);
}

/* ---------------- 页操作原语 ---------------- */

/*
 * 分配一页并初始化成 blob 页。
 *
 * 返回的 frame 已 pin，调用方负责 unpin。
 */
static int blob_alloc_page(BlobStore *bs, u32 *pid_out, PageFrame **frame_out)
{
    PageFrame *f;
    u32 pid;
    int rc = pager_alloc(bs->pager, &pid, &f);
    if (rc != OK)
        return rc;

    rc = page_init(f->data, PT_BLOB);
    if (rc != OK) {
        pager_unpin(bs->pager, pid, false);
        return rc;
    }

    *pid_out = pid;
    *frame_out = f;
    return OK;
}

/*
 * 把一个页的内容写进磁盘（先写日志，再改页）。
 *
 * before_pg / after_pg 是整页镜像。
 * 调用后 page 已 unpin。
 */
static int blob_commit_page(BlobStore *bs, u32 xid, u32 pid,
                            const u8 *before_pg, const u8 *after_pg)
{
    PageFrame *f;
    int rc = pager_get(bs->pager, pid, &f);
    if (rc != OK)
        return rc;

    u32 lsn = 0;
    rc = blob_log_page(bs, xid, pid, before_pg, after_pg, &lsn);
    if (rc != OK) {
        pager_unpin(bs->pager, pid, false);
        return rc;
    }

    /* 改页：整页盖上去 */
    memcpy(f->data, after_pg, PAGE_SIZE);
    if (lsn)
        page_set_lsn(f->data, lsn);
    page_set_checksum(f->data);

    return pager_unpin(bs->pager, pid, true);
}

/* ---------------- 创建对象 ---------------- */

int blob_create(BlobStore *bs, u64 nframes, u32 frame_size, BlobId *blob_id_out)
{
    if (!bs || !blob_id_out)
        return E_INVAL;
    if (nframes == 0)
        return E_INVAL;
    if (frame_size == 0 || frame_size > PAGE_SIZE - HDR_SIZE)
        return E_INVAL;

    /* 一个索引块能覆盖多少帧 */
    u64 per_index = BLOB_FRAMES_PER_INDEX;
    u64 n_index_blk = (nframes + per_index - 1) / per_index;
    if (n_index_blk > BLOB_MAX_INDEX_BLOCKS)
        return E_NOSPACE;

    /*
     * 分配页：头页 + 帧页 + 索引块页。
     *
     * 【必须连续分配】
     *
     * 帧号 → 页号靠的是 base_page + N。
     * 如果中间插了别的页，映射就错了 —— 读到别的对象的数据。
     *
     * pager_alloc 是顺序分配文件末尾的页，所以连续调用它就得到连续页号。
     * 但**不能中途失败**：如果第 5 页分配失败，前 4 页已经分配出去了，
     * 对象就不完整了。
     *
     * 处理办法：先全部分配完再统一写内容。
     * 分配失败的话，把已分配的页留着（成为"泄漏的空洞"）——
     * 宁可浪费空间，也不能让对象处于"部分存在"的状态。
     * 这比"回滚已分配的页"简单得多，且不会产生数据不一致。
     */
    u64 total_pages = 1 + nframes + n_index_blk;
    if (total_pages > 0xFFFFFFFFull)
        return E_NOSPACE;

    u32 head_pid = PAGE_ID_NIL;
    u32 *frame_pids = NULL;
    u32 *index_pids = NULL;

    frame_pids = malloc((size_t)nframes * sizeof(u32));
    index_pids = malloc((size_t)n_index_blk * sizeof(u32));
    if (!frame_pids || !index_pids) {
        free(frame_pids);
        free(index_pids);
        return E_IO;
    }

    int rc = OK;
    u32 pid;
    PageFrame *f;

    /* 头页 */
    rc = blob_alloc_page(bs, &head_pid, &f);
    if (rc != OK)
        goto fail;

    /*
     * 立刻 unpin 头页（v0.10 修的真 bug）。
     *
     * blob_alloc_page 返回的帧是**被 pin 住**的（调用方负责 unpin）。
     * 帧页和索引块页都 unpin 了，唯独头页漏了 ——
     * 于是**每建一个对象就泄漏一个 pin**。
     *
     * 后果：缓冲池的帧是有限的（256 帧），
     * 建满 256 个对象后，所有帧的 pin_count 都 > 0
     * → pick_victim 找不到可逐出的帧 → pager_alloc 返回 E_NOSPACE
     * → "空间不足"。
     *
     * 【为什么之前没发现】
     * 之前的测试最多建几个对象，撑不到 256 帧。
     * 是 v0.10 的并发测试把强度提上去（建 1280 个对象）才暴露出来。
     *
     * 【为什么可以在这里 unpin】
     * 头页的内容是在后面 blob_commit_page 里写的，
     * 那一步自己会 pager_get + pager_unpin ✓。
     * 这里 unpin 不影响任何事。
     */
    pager_unpin(bs->pager, head_pid, true);

    /* 帧页：连续分配 */
    for (u64 i = 0; i < nframes; i++) {
        rc = blob_alloc_page(bs, &pid, &f);
        if (rc != OK)
            goto fail;
        frame_pids[i] = pid;
        pager_unpin(bs->pager, pid, true);
    }

    /* 索引块页 */
    for (u64 i = 0; i < n_index_blk; i++) {
        rc = blob_alloc_page(bs, &pid, &f);
        if (rc != OK)
            goto fail;
        index_pids[i] = pid;
        pager_unpin(bs->pager, pid, true);
    }

    /*
     * 校验连续性 —— 这是"帧号→页号纯算术"的**前提**。
     *
     * 如果不连续（比如将来 pager_alloc 改了策略），映射就错了，
     * 会读到别的对象的数据 —— 静默的数据损坏。
     *
     * 所以这里必须检查，而不是"假设它连续"。
     */
    for (u64 i = 0; i < nframes; i++) {
        if (frame_pids[i] != frame_pids[0] + i) {
            rc = E_CORRUPT;
            goto fail;
        }
    }

    /* 写头页 */
    {
        BlobHdr h;
        memset(&h, 0, sizeof(h));
        h.nframes     = nframes;
        h.frame_size  = frame_size;
        h.total_len   = 0;          /* 写数据时才定 */
        h.base_page   = frame_pids[0];
        h.n_index_blk = (u32)n_index_blk;
        for (u64 i = 0; i < n_index_blk; i++)
            h.index_blk[i] = index_pids[i];

        u8 before[PAGE_SIZE];
        u8 after[PAGE_SIZE];

        /* before：头页现在的内容（刚分配，全 0 的页头） */
        rc = pager_get(bs->pager, head_pid, &f);
        if (rc != OK)
            goto fail;
        memcpy(before, f->data, PAGE_SIZE);
        pager_unpin(bs->pager, head_pid, false);

        memcpy(after, before, PAGE_SIZE);
        blob_hdr_write(after, &h);

        /* 标记这是「头页」—— 帧页没有这个标志。
         * 重建目录时靠它把头页和帧页区分开。 */
        after[H_FLAGS] |= BLOB_FLAG_HEAD;

        page_set_checksum(after);

        rc = blob_commit_page(bs, 0, head_pid, before, after);
        if (rc != OK)
            goto fail;
    }

    /* 写索引块：帧号 → 页号 */
    for (u64 b = 0; b < n_index_blk; b++) {
        u8 before[PAGE_SIZE];
        u8 after[PAGE_SIZE];

        rc = pager_get(bs->pager, index_pids[b], &f);
        if (rc != OK)
            goto fail;
        memcpy(before, f->data, PAGE_SIZE);
        pager_unpin(bs->pager, index_pids[b], false);

        memcpy(after, before, PAGE_SIZE);
        u8 *idx = after + HDR_SIZE;
        for (u32 k = 0; k < BLOB_FRAMES_PER_INDEX; k++) {
            u64 frame_no = b * BLOB_FRAMES_PER_INDEX + k;
            u32 v = (frame_no < nframes) ? frame_pids[frame_no] : PAGE_ID_NIL;
            put_u32(idx + 4u * k, v);
        }
        page_set_checksum(after);

        rc = blob_commit_page(bs, 0, index_pids[b], before, after);
        if (rc != OK)
            goto fail;
    }

    free(frame_pids);
    free(index_pids);

    bs->stats.n_objects++;
    bs->stats.n_frames += nframes;

    BlobId id = (BlobId)head_pid + 1;
    rc = blob_dir_push(bs, id);
    if (rc != OK)
        return rc;

    *blob_id_out = id;
    return OK;

fail:
    /*
     * 失败处理：已分配的页就留着当空洞。
     *
     * 为什么不做回滚：回滚要记得每个页的 before 内容，
     * 而且回滚本身也可能失败 → 半回滚状态更难收拾。
     * 泄漏几个空洞页 vs. 对象处于半存在状态，前者安全得多。
     */
    free(frame_pids);
    free(index_pids);
    return rc;
}

/* ---------------- 读对象头 ---------------- */

/*
 * 读对象头页并解析。
 *
 * 头页会读到 buffer pool 的缓存里 —— 反复读同一个对象的头页，
 * 第二次起就是缓存命中，IO 量 ≈ 0。
 */
static int blob_load_hdr(BlobStore *bs, BlobId id, BlobHdr *h)
{
    if (id == BLOB_ID_INVALID)
        return E_NOTFOUND;

    u32 head = blob_head_page(id);

    PageFrame *f;
    int rc = pager_get(bs->pager, head, &f);
    if (rc != OK)
        return rc;

    /*
     * 判据：这个页是不是 blob 头页？
     *
     * 【为什么是 NOTFOUND 而不是 CORRUPT】
     *
     * 页号空间是**共享**的 —— 关系模态的表页、catalog 页也占页号。
     * 而 blob_id = 头页页号 + 1，所以 blob_id=1 可能指向任何页。
     *
     * 「这个 id 不是一个 blob 对象」是**正常情况**（用户传了个无效 id），
     * 不是数据损坏。返回 CORRUPT 会让上层显示"数据损坏"，
     * 把用户的误用说成库坏了 —— 误导排查方向。
     *
     * 真正的 CORRUPT 是：类型对、标志对，但内容坏了（magic 不对 / crc 不对）。
     */
    if (f->data[H_TYPE] != PT_BLOB ||
        (f->data[H_FLAGS] & BLOB_FLAG_HEAD) == 0) {
        pager_unpin(bs->pager, head, false);
        return E_NOTFOUND;
    }

    rc = blob_hdr_parse(f->data, h);
    pager_unpin(bs->pager, head, false);
    return rc;
}

/* ---------------- 写对象 ---------------- */

int blob_write(BlobStore *bs, u32 xid, BlobId id, const u8 *data, u64 len)
{
    if (!bs || (!data && len > 0))
        return E_INVAL;

    BlobHdr h;
    int rc = blob_load_hdr(bs, id, &h);
    if (rc != OK)
        return rc;

    u64 capacity = h.nframes * h.frame_size;
    if (len > capacity)
        return E_NOSPACE;

    /* 写每一帧 */
    u64 written = 0;
    for (u64 i = 0; i < h.nframes; i++) {
        u64 remain = (len > written) ? (len - written) : 0;
        u32 n = (remain < h.frame_size) ? (u32)remain : h.frame_size;

        u32 pid = h.base_page + (u32)i;

        u8 before[PAGE_SIZE];
        u8 after[PAGE_SIZE];

        PageFrame *f;
        rc = pager_get(bs->pager, pid, &f);
        if (rc != OK)
            return rc;
        memcpy(before, f->data, PAGE_SIZE);
        pager_unpin(bs->pager, pid, false);

        memcpy(after, before, PAGE_SIZE);
        memset(after + HDR_SIZE, 0, PAGE_SIZE - HDR_SIZE);
        if (n)
            memcpy(after + HDR_SIZE, data + written, n);

        /*
         * 帧页里写上自己的帧号 —— 读的时候校验。
         *
         * 为什么校验：万一页号算错（映射的前提不成立），
         * 我们会读到**别的帧**甚至**别的对象**的数据。
         * 帧号对不上 → 立刻报错，而不是把错误数据返回给用户。
         *
         * 帧号存在帧数据区开头的 8 字节（不占用户数据的空间？
         * 不行 —— 用户数据要占满整个帧区）。
         *
         * 换个位置：帧号存页头的 H_NEXT 字段（blob 页不用它）。
         * H_NEXT 是 u32，帧号用 u32 够（一个对象不会有 40 亿帧）。
         */
        put_u32(after + H_NEXT, (u32)i);
        page_set_checksum(after);

        rc = blob_commit_page(bs, xid, pid, before, after);
        if (rc != OK)
            return rc;

        written += n;
    }

    /* 更新对象头：total_len 定稿 */
    {
        u8 before[PAGE_SIZE];
        u8 after[PAGE_SIZE];

        u32 head = blob_head_page(id);

        PageFrame *f;
        rc = pager_get(bs->pager, head, &f);
        if (rc != OK)
            return rc;
        memcpy(before, f->data, PAGE_SIZE);
        pager_unpin(bs->pager, head, false);

        memcpy(after, before, PAGE_SIZE);
        BlobHdr nh;
        rc = blob_hdr_parse(after, &nh);
        if (rc != OK)
            return rc;
        nh.total_len = len;
        blob_hdr_write(after, &nh);
        page_set_checksum(after);

        rc = blob_commit_page(bs, xid, head, before, after);
        if (rc != OK)
            return rc;
    }

    bs->stats.bytes_written += len;
    return OK;
}

/* ---------------- 读 ---------------- */

/*
 * 读第 frame_no 帧。
 *
 * IO 路径：
 *   1. 读头页（缓存命中率高）
 *   2. 帧号 → 页号：纯算术 base_page + N
 *   3. 读帧页
 *
 * 为什么不查索引块：帧号→页号是算术（见 blob.h 的设计说明）。
 * 索引块是为"页不连续"准备的，当前用不上。
 */
int blob_read_frame(BlobStore *bs, BlobId id, u64 frame_no,
                    u8 *buf, u32 cap, u32 *len_out)
{
    if (!bs || !buf)
        return E_INVAL;

    BlobHdr h;
    int rc = blob_load_hdr(bs, id, &h);
    if (rc != OK)
        return rc;

    if (frame_no >= h.nframes)
        return E_NOTFOUND;      /* 越界：不能返回垃圾 */

    u32 pid = h.base_page + (u32)frame_no;

    PageFrame *f;
    rc = pager_get(bs->pager, pid, &f);
    if (rc != OK)
        return rc;

    bs->stats.page_reads++;

    /* 帧页自校验：页里的帧号必须对得上 */
    if (f->data[H_TYPE] != PT_BLOB || get_u32(f->data + H_NEXT) != (u32)frame_no) {
        pager_unpin(bs->pager, pid, false);
        return E_CORRUPT;
    }

    /* 这一帧有多少字节 */
    u64 start = frame_no * h.frame_size;
    u32 n;
    if (start >= h.total_len) {
        n = 0;                        /* 对象没写到这个位置 */
    } else {
        u64 remain = h.total_len - start;
        n = (remain < h.frame_size) ? (u32)remain : h.frame_size;
    }

    if (n > cap) {
        pager_unpin(bs->pager, pid, false);
        return E_NOSPACE;
    }

    if (n)
        memcpy(buf, f->data + HDR_SIZE, n);

    pager_unpin(bs->pager, pid, false);

    if (len_out)
        *len_out = n;
    bs->stats.bytes_read += n;
    return OK;
}

int blob_read(BlobStore *bs, BlobId id, u8 *buf, u64 cap, u64 *len_out)
{
    if (!bs || !buf)
        return E_INVAL;

    BlobHdr h;
    int rc = blob_load_hdr(bs, id, &h);
    if (rc != OK)
        return rc;

    if (h.total_len > cap)
        return E_NOSPACE;

    u64 written = 0;
    for (u64 i = 0; i < h.nframes; i++) {
        u64 start = i * h.frame_size;
        if (start >= h.total_len)
            break;

        u32 n;
        u64 remain = h.total_len - start;
        n = (remain < h.frame_size) ? (u32)remain : h.frame_size;

        rc = blob_read_frame(bs, id, i, buf + written, (u32)(cap - written), &n);
        if (rc != OK)
            return rc;
        written += n;
    }

    if (len_out)
        *len_out = written;
    return OK;
}

int blob_stat(BlobStore *bs, BlobId id,
              u64 *nframes, u32 *frame_size, u64 *total_len)
{
    if (!bs)
        return E_INVAL;

    BlobHdr h;
    int rc = blob_load_hdr(bs, id, &h);
    if (rc != OK)
        return rc;

    if (nframes)
        *nframes = h.nframes;
    if (frame_size)
        *frame_size = h.frame_size;
    if (total_len)
        *total_len = h.total_len;
    return OK;
}

int blob_header(BlobStore *bs, BlobId id,
                u64 *nframes, u32 *frame_size, u64 *total_len, u32 *base_page)
{
    if (!bs)
        return E_INVAL;

    BlobHdr h;
    int rc = blob_load_hdr(bs, id, &h);
    if (rc != OK)
        return rc;

    if (nframes)
        *nframes = h.nframes;
    if (frame_size)
        *frame_size = h.frame_size;
    if (total_len)
        *total_len = h.total_len;
    if (base_page)
        *base_page = h.base_page;
    return OK;
}

/*
 * 取第 frame_no 帧的定位信息（不读帧内容）。
 *
 * 帧页号 = base_page + frame_no（纯算术，见 blob.h 的设计说明）。
 * 文件偏移 = 页号 * PAGE_SIZE + 页头长度（页在文件里是定长对齐的）。
 */
int blob_frame_info(BlobStore *bs, BlobId id, u64 frame_no,
                    u64 *off_out, u32 *len_out)
{
    if (!bs)
        return E_INVAL;

    BlobHdr h;
    int rc = blob_load_hdr(bs, id, &h);
    if (rc != OK)
        return rc;

    if (frame_no >= h.nframes)
        return E_NOTFOUND;

    if (off_out) {
        u64 page = (u64)h.base_page + frame_no;
        *off_out = page * (u64)PAGE_SIZE + (u64)HDR_SIZE;
    }

    if (len_out) {
        u64 start = frame_no * (u64)h.frame_size;
        u32 n;
        if (start >= h.total_len) {
            n = 0;                        /* 对象没写到这个位置 */
        } else {
            u64 remain = h.total_len - start;
            n = (remain < h.frame_size) ? (u32)remain : h.frame_size;
        }
        *len_out = n;
    }

    return OK;
}

const BlobStats *blob_stats(const BlobStore *bs)
{
    return bs ? &bs->stats : NULL;
}
