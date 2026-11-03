/*
 * repl_ft.c — REPL 的 FT 命令（v1.3 步骤 4）
 *
 * 设计说明见 repl_ft.h。
 *
 * 【为什么检索要走命令而不是 SQL】
 *
 * 相关性排序（BM25）依赖全局统计量，执行器算不出来 ——
 * 这正是 PLAN-v1.3 §1.2 的判决。所以 FT SEARCH 是**专用命令**，
 * 和 VEC KNN 一样。
 *
 * 【参数怎么切】
 *
 *   FT INDEX <doc_id> <text>     doc_id 是第一个 token，剩下的**整段**都是正文
 *   FT SEARCH <query> <top_k>    top_k 是**最后一个** token，前面整段都是查询串
 *
 * 为什么 SEARCH 的 top_k 放最后：查询串里可能有空格
 * （"hello world" 是一个查询，不是两个参数）。
 * 为什么 INDEX 的 doc_id 放最前：正文是自由文本，只能从后面切。
 */

#include "repl_ft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 去掉行尾的换行符（返回正文的长度） */
static size_t trim_eol(const char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        n--;
    }
    return n;
}

/* 跳过行首的空白 */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

int handle_ft(DB *db, char *line)
{
    char *endp = NULL;
    long v;

    line = (char *)skip_ws(line);
    if (strncmp(line, "FT", 2) != 0) {
        return 0;
    }
    line += 2;
    line = (char *)skip_ws(line);

    /* ---------------- FT INDEX <doc_id> <text> ---------------- */
    if (strncmp(line, "INDEX", 5) == 0) {
        size_t tlen;

        line += 5;
        line = (char *)skip_ws(line);

        v = strtol(line, &endp, 10);
        if (endp == line || v < 0) {
            printf("用法: FT INDEX <doc_id> <text>\n");
            return 1;
        }
        line = (char *)skip_ws(endp);

        tlen = trim_eol(line);
        if (tlen == 0) {
            printf("用法: FT INDEX <doc_id> <text>\n");
            return 1;
        }

        db_err e = db_ft_add(db, (uint32_t)v, line, (uint32_t)tlen);
        if (e != DB_OK) {
            printf("灌文档失败: %s\n", db_strerror(e));
        } else {
            printf("已索引 doc_id=%ld（%zu 字节）\n", v, tlen);
        }
        return 1;
    }

    /* ---------------- FT SEARCH <query> <top_k> ---------------- */
    if (strncmp(line, "SEARCH", 6) == 0) {
        char *qstart;
        size_t qlen;
        long top_k;
        db_ft_hit *hits;
        uint32_t n = 0;
        db_err e;

        line += 6;
        line = (char *)skip_ws(line);
        if (*line == '\0' || *line == '\n' || *line == '\r') {
            printf("用法: FT SEARCH <query> <top_k>\n");
            return 1;
        }

        /*
         * top_k 是**最后一个** token：先找行尾那个 token，
         * 再把前面整段当查询串。
         *
         * 做法：从行尾往前找最后一个空白，它前面那段就是 top_k。
         */
        {
            char *p = line + trim_eol(line);
            char *last_ws = NULL;

            while (p > line) {
                p--;
                if (*p == ' ' || *p == '\t') {
                    last_ws = p;
                    break;
                }
            }
            if (!last_ws) {
                /* 只有一个 token：它是 top_k，查询串为空 */
                printf("用法: FT SEARCH <query> <top_k>\n");
                return 1;
            }
            *last_ws = '\0';       /* 把 top_k 切下来 */
            v = strtol(last_ws + 1, &endp, 10);
            if (endp == last_ws + 1 || v <= 0) {
                printf("用法: FT SEARCH <query> <top_k>\n");
                return 1;
            }
            top_k = v;
        }

        qstart = line;
        qlen = trim_eol(qstart);
        if (qlen == 0) {
            printf("用法: FT SEARCH <query> <top_k>\n");
            return 1;
        }

        hits = (db_ft_hit *)calloc((size_t)top_k, sizeof(db_ft_hit));
        if (!hits) {
            printf("内存不足\n");
            return 1;
        }

        e = db_ft_search(db, qstart, (uint32_t)top_k, hits,
                         (uint32_t)top_k, &n);
        if (e != DB_OK) {
            printf("检索失败: %s\n", db_strerror(e));
        } else {
            for (uint32_t i = 0; i < n; i++) {
                printf("  doc_id=%u  score=%.6f\n", hits[i].doc_id,
                       hits[i].score);
            }
            printf("(%u 个)\n", n);
        }
        free(hits);
        return 1;
    }

    printf("用法: FT INDEX / FT SEARCH\n");
    return 1;
}
