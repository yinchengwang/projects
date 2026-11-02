/*
 * gstore_cursor.c — 定长槽页的拉式游标（v0.11 步骤 3）
 *
 * 设计说明见 storage/gstore.h。
 *
 * 【为什么需要它】
 *
 * gstore_foreach 是**推式**：一次性走完整个 gstore，访问每条记录。
 * 但 SPI 的行源是**拉式**的 —— "给我下一行"。
 *
 * 执行器的扫描主循环长这样：
 *
 *     while (scan_next(...) == OK) { ...处理这一行... }
 *
 * 它不会先攒齐再给。所以适配器需要"一条一条来"的能力。
 *
 * 【为什么不把游标塞进 gstore.c】
 *
 * gstore.c 现在 713 行，游标实现再加 ~90 行会逼近 800 行的红线。
 * 而且游标和分配/恢复是两件事：游标只读，分配/恢复要写 WAL。
 * 分开之后，"改游标不会碰到写路径"这件事是物理隔离的，不是靠自觉。
 *
 * 【游标的状态只有两个字段】
 *
 *   page —— 当前页
 *   slot —— 当前页内下一个要检查的槽
 *
 * 就这两个。槽的位置是纯算术（20 + i * rec_size），
 * 所以"下一个"不需要额外的链或索引 —— 顺序扫即可。
 *
 * 【已释放的槽怎么办】
 *
 * 跳过。槽被释放后 in_use = 0，但它还在页里（空闲链挂着）。
 * 遍历时看到 in_use = 0 就当它不存在。
 *
 * 【并发约束】
 *
 * 游标只在单语句内有效。语句执行期间如果有并发写释放了槽，
 * 游标可能看到"记录被删又建"的复用 —— 这是图模态已记录的代价
 * （PLAN §1.4："空闲记录立即复用 + 无版本链 = 读游标跨语句使用时
 * 可能看到记录被复用"）。缓解：游标不跨语句。
 */

#include "storage/gstore.h"

#include <string.h>

/* ---------------- 初始化 ---------------- */

void gstore_cursor_init(GStore *gs, GStoreCursor *c)
{
    if (!gs || !c)
        return;

    /* 从页链头开始。空库（head == PAGE_ID_NIL）→ 第一次 next 直接 E_EOF */
    c->page = gs->head_page;
    c->slot = 0;
}

/* ---------------- 取下一条 ---------------- */

int gstore_cursor_next(GStore *gs, GStoreCursor *c, Rid *rid,
                       u8 *rec, u16 cap, u16 *len)
{
    if (!gs || !c || !rec)
        return E_INVAL;

    /* 容量不够 —— 调用方据此换大缓冲（SPI 的通用约定） */
    if (cap < gs->rec_size)
        return E_NOSPACE;

    while (c->page != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        int rc = pager_get(gs->pg, c->page, &fr);
        if (rc != OK)
            return rc;

        /*
         * 页类型 / 记录大小必须与本 gstore 一致。
         *
         * 不一致说明页链被串错了（两种 gstore 的页混进同一条链）。
         * 这不是"读不到"而是"数据坏了" → E_CORRUPT，不能静默跳过。
         */
        if (fr->data[GS_H_TYPE] != gs->page_type ||
            gs_rec_size(fr->data) != gs->rec_size) {
            pager_unpin(gs->pg, c->page, false);
            return E_CORRUPT;
        }

        u16 nslots = gs_nslots(fr->data);
        u16 i = c->slot;

        for (; i < nslots; i++) {
            const u8 *r = fr->data + gs_slot_off(fr->data, i);

            /* 已释放的槽：跳过（它还在页里，但在空闲链上） */
            if (!gs_in_use(r))
                continue;

            memcpy(rec, r, gs->rec_size);
            if (len)
                *len = gs->rec_size;
            if (rid)
                *rid = rid_make(c->page, i);

            /* 游标停在"下一个要检查的槽" */
            c->slot = (u16)(i + 1);

            gs->rec_reads++;
            pager_unpin(gs->pg, c->page, false);
            return OK;
        }

        /* 本页扫完 → 顺着页链去下一页 */
        u32 next = gs_next(fr->data);
        pager_unpin(gs->pg, c->page, false);

        c->page = next;
        c->slot = 0;
    }

    return E_EOF;      /* 走完了 */
}
