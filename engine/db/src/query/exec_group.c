/*
 * exec_group.c — 组键编码 + hash 组表（v1.1 步骤 3）
 *
 * ==================== 这一层负责什么 ====================
 *
 * 把一行变成"组"：
 *   1. 算组键（各分组列的值依次编码）
 *   2. 在 hash 表里找这个组；没有就建
 *
 * 聚合本身（在组里累加）在 exec_agg.c。
 *
 * ==================== 组键 ====================
 *
 * 组键 = 各分组列的值依次编码。
 *
 * 【为什么要带类型字节】
 * 不带的话，INT 1 和 TEXT "1" 的键相同 → 会被分到同一组 → 结果错。
 *
 * 【为什么要"字节序 = 数值序"】
 * 输出按键排序（保证结果确定），所以编码的字节序必须等于值的序 ✓。
 * 整数用大端 + 翻转符号位，浮点用符号位翻转，文本用原字节 + 转义 + 终止符。
 *
 * 【为什么要转义 + 终止符】
 * 文本里可以有任意字节（包括 0x00）。不转义的话
 *   "a" + "b"  与  "ab" + ""   的键会一样 → 不同的组被合并 → 结果错。
 * 转义（0x00 → 0x00 0xFF）保证值部分互不为前缀，
 * 终止符（0x00 0x00）保证值部分结束。
 *
 * 【外部值（blob 模态）怎么办】
 * 外部值的内容不在批的文本池里 —— 必须先物化才能编码。
 * 物化失败就报错（不给错答案）。
 */

#include "query/exec_group.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 组键缓冲的上限：一列最多 1（类型）+ 2×255（转义后）+ 2（终止符） */
#define KEY_COL_MAX  (1u + 2u * (VAL_TEXT_MAX - 1u) + 2u)
#define KEY_BUF_MAX  (KEY_COL_MAX * SQL_MAX_COLS)

/* ---------------- 组键编码 ---------------- */

/*
 * 把一行的一个分组列编码进键缓冲。
 * 成功返回 1，失败返回 0（缓冲不够 / 外部值读不出来）。
 */
static int key_put_col(const BatchCtx *bc, int colidx, u32 row,
                       u8 *buf, u32 cap, u32 *off)
{
    const AmBatchCol *col = &bc->col[colidx];

    /* NULL：类型字节 0（排在最前，与 SQLite 的 NULL < 数值 < 文本 一致） */
    if (bit_get(col->nulls, row)) {
        if (*off + 1u > cap)
            return 0;
        buf[(*off)++] = 0;
        return 1;
    }

    /* 外部值：物化后按文本编码 */
    if (bit_get(col->extflag, row)) {
        DbValue v;
        u8 *tmp = NULL;
        u32 len = 0;

        batch_cell(bc, colidx, row, &v);
        if (val_detoast(&v, &tmp, &len) != OK)
            return 0;

        if (*off + 1u > cap) { free(tmp); return 0; }
        buf[(*off)++] = 3;
        for (u32 i = 0; i < len; i++) {
            if (*off + 2u > cap) { free(tmp); return 0; }
            u8 ch = tmp[i];
            buf[(*off)++] = ch;
            if (ch == 0x00)
                buf[(*off)++] = 0xFF;      /* 转义 */
        }
        free(tmp);

        if (*off + 2u > cap)
            return 0;
        buf[(*off)++] = 0x00;             /* 终止符 */
        buf[(*off)++] = 0x00;
        return 1;
    }

    switch (col->type) {
    case T_INT4:
    case T_INT8:
    case T_BOOL: {
        if (*off + 9u > cap)
            return 0;
        buf[(*off)++] = 1;
        u64 u = (u64)col->ints[row] ^ 0x8000000000000000ull;  /* 翻转符号位 */
        for (int i = 0; i < 8; i++)
            buf[(*off)++] = (u8)(u >> (56 - 8 * i));
        return 1;
    }
    case T_REAL: {
        if (*off + 9u > cap)
            return 0;
        buf[(*off)++] = 2;
        double d = col->reals[row];
        if (d == 0.0)
            d = 0.0;               /* 归一化 -0.0 → +0.0（数值相等要同组） */
        u64 bits;
        memcpy(&bits, &d, 8);
        u64 k = (bits & 0x8000000000000000ull) ? ~bits
                  : (bits | 0x8000000000000000ull);
        for (int i = 0; i < 8; i++)
            buf[(*off)++] = (u8)(k >> (56 - 8 * i));
        return 1;
    }
    default: {                    /* T_TEXT */
        if (*off + 1u > cap)
            return 0;
        buf[(*off)++] = 3;
        u16 n = col->tlen[row];
        const u8 *src = col->tpool + col->toff[row];
        for (u16 i = 0; i < n; i++) {
            if (*off + 2u > cap)
                return 0;
            u8 ch = src[i];
            buf[(*off)++] = ch;
            if (ch == 0x00)
                buf[(*off)++] = 0xFF;      /* 转义 */
        }
        if (*off + 2u > cap)
            return 0;
        buf[(*off)++] = 0x00;             /* 终止符 */
        buf[(*off)++] = 0x00;
        return 1;
    }
    }
}

/* FNV-1a */
static u32 hash_bytes(const u8 *p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* ---------------- 组表 ---------------- */

int group_tab_init(GroupTab *gt, const GroupCols *cols, int naggs)
{
    memset(gt, 0, sizeof(*gt));
    gt->cols = *cols;
    gt->naggs = naggs;

    gt->tabcap = 64;
    gt->tab = calloc(gt->tabcap, sizeof(Group));
    if (!gt->tab)
        return E_IO;

    gt->keycap = KEY_BUF_MAX;
    gt->keybuf = malloc(gt->keycap);
    if (!gt->keybuf)
        return E_IO;

    return OK;
}

void group_tab_free(GroupTab *gt)
{
    if (gt->tab) {
        for (u32 i = 0; i < gt->tabcap; i++) {
            if (gt->tab[i].used) {
                free(gt->tab[i].key);
                free(gt->tab[i].acc);
                free(gt->tab[i].rep);
            }
        }
        free(gt->tab);
    }
    free(gt->keybuf);
    memset(gt, 0, sizeof(*gt));
}

/*
 * 扩容（rehash）。
 *
 * 【为什么必须扩容】
 * 表固定大小的话，组数超过容量就再也放不进去了。
 * 开放寻址在表满时会无限探查 —— 那是死循环，不是报错。
 * 所以到 75% 负载就翻倍。
 */
static int group_grow(GroupTab *gt)
{
    u32 ncap = gt->tabcap * 2u;
    if (ncap > (1u << 20))
        return E_NOSPACE;          /* 不让它无限涨 */

    Group *nt = calloc(ncap, sizeof(Group));
    if (!nt)
        return E_IO;

    u32 mask = ncap - 1u;
    for (u32 i = 0; i < gt->tabcap; i++) {
        Group *g = &gt->tab[i];
        if (!g->used)
            continue;
        u32 h = hash_bytes(g->key, g->keylen) & mask;
        while (nt[h].used)
            h = (h + 1u) & mask;
        nt[h] = *g;                /* 转移（指针跟着走） */
    }

    free(gt->tab);
    gt->tab = nt;
    gt->tabcap = ncap;
    return OK;
}

int group_slot(GroupTab *gt, const BatchCtx *bc, u32 row, int *isnew)
{
    /* 算组键 */
    u32 klen = 0;
    for (int c = 0; c < gt->cols.ncols; c++) {
        if (!key_put_col(bc, gt->cols.col[c], row,
                         gt->keybuf, gt->keycap, &klen))
            return -2;             /* 键编码失败（文本过长 / 外部值读失败） */
    }
    const u8 *key = gt->keybuf;
    u32 keylen = klen;

    /* 负载到 75% 就扩容 */
    if ((gt->ngroups + 1u) * 4u >= gt->tabcap * 3u) {
        int rc = group_grow(gt);
        if (rc != OK)
            return -2;
    }

    u32 mask = gt->tabcap - 1u;
    u32 h = hash_bytes(key, keylen) & mask;

    for (u32 probe = 0; probe <= mask; probe++) {
        u32 i = (h + probe) & mask;
        Group *g = &gt->tab[i];

        if (!g->used) {
            /* 新组 */
            if (gt->ngroups >= MAX_GROUPS)
                return -1;                  /* 超限 */

            memset(g, 0, sizeof(*g));
            g->used = true;
            g->key = malloc(keylen ? keylen : 1);
            /* calloc(0, ...) 可能返回 NULL —— 统一按 1 个元素申请 */
            g->acc = calloc((size_t)(gt->naggs ? gt->naggs : 1), sizeof(AggAcc));
            g->rep = calloc((size_t)(gt->cols.ncols ? gt->cols.ncols : 1),
                            sizeof(DbValue));
            if (!g->key || !g->acc || !g->rep)
                return -2;

            if (keylen)
                memcpy(g->key, key, keylen);
            g->keylen = keylen;

            /* 代表行：分组列的值（输出用） */
            for (int c = 0; c < gt->cols.ncols; c++)
                batch_cell(bc, gt->cols.col[c], row, &g->rep[c]);

            gt->ngroups++;
            *isnew = 1;
            return (int)i;
        }

        if (g->keylen == keylen && memcmp(g->key, key, keylen) == 0) {
            *isnew = 0;
            return (int)i;
        }
    }
    return -2;
}
