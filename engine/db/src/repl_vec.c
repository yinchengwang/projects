/*
 * repl_vec.c — REPL 的 VEC 命令（v1.0 步骤 5）
 *
 * 设计说明见 repl_vec.h。
 */

#include "repl_vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * VEC 命令。
 *
 *   VEC CREATE <dim> [M] [EFC]        建向量存储
 *   VEC INSERT <label> <v0> ... <vN>   插入一个向量
 *   VEC KNN <k> <v0> ... <vN>          近邻查询（ANN）
 *   VEC KNN EXACT <k> <v0> ... <vN>   精确近邻（暴力扫）
 *   VEC STAT                          统计
 *   VEC FLUSH                         索引落盘
 *
 * 向量分量的个数必须等于维度 —— 少给不给都不行：
 * "少给"会让维度和磁盘上的对不上，"不给"分不清是 0 还是漏了。
 */
int handle_vec(DB *db, char *line)
{
    while (*line == ' ')
        line++;
    if (strncmp(line, "VEC", 3) != 0)
        return 0;
    line += 3;
    while (*line == ' ')
        line++;

    if (strncmp(line, "CREATE", 6) == 0) {
        line += 6;

        /*
         * 参数按顺序取：dim [M] [EFC]。
         *
         * 【坑】
         *
         * 第一版用 `strtol(line, NULL, 10)` 取 dim，然后**又从 line 重新扫**
         * 找 M —— 于是 dim 被读了两次（"VEC CREATE 4" 会变成 dim=4, M=4）。
         *
         * 正确做法：每次取参数都用 strtol 的 endp 把游标往前推，
         * 游标只前进不回退。
         */
        char *endp = NULL;
        long dim = strtol(line, &endp, 10);
        if (endp == line || dim <= 0) {
            printf("用法: VEC CREATE <dim> [M] [EFC]\n");
            return 1;
        }
        line = endp;

        int m = 0;
        int efc = 0;

        while (*line == ' ' || *line == '\t')
            line++;
        if (*line && *line != '\n' && *line != '\r') {
            m = (int)strtol(line, &endp, 10);
            if (endp == line)
                m = 0;
            line = endp;
        }

        while (*line == ' ' || *line == '\t')
            line++;
        if (*line && *line != '\n' && *line != '\r') {
            efc = (int)strtol(line, &endp, 10);
            if (endp == line)
                efc = 0;
            line = endp;
        }

        db_err e = db_vec_create(db, (uint16_t)dim, (uint16_t)m,
                                 (uint16_t)efc);
        if (e != DB_OK)
            printf("建向量存储失败: %s\n", db_strerror(e));
        else
            printf("向量存储已创建（维度 %ld，M=%d，EFC=%d）\n", dim,
                   m ? m : 16, efc ? efc : 200);
        return 1;
    }

    if (strncmp(line, "INSERT", 6) == 0) {
        line += 6;

        if (!db_vec_exists(db)) {
            printf("还没有向量存储（先 VEC CREATE）\n");
            return 1;
        }

        char *endp = NULL;
        long label = strtol(line, &endp, 10);
        if (endp == line) {
            printf("用法: VEC INSERT <label> <v0> ... <vN>\n");
            return 1;
        }

        uint16_t dim = db_vec_dim(db);
        float *vec = (float *)calloc(dim, sizeof(float));
        if (!vec)
            return 1;

        int got = 0;
        line = endp;
        while (*line) {
            while (*line == ' ' || *line == ',')
                line++;
            if (!*line || *line == '\n' || *line == '\r')
                break;
            char *e2 = NULL;
            double v = strtod(line, &e2);
            if (e2 == line)
                break;
            if (got < dim)
                vec[got] = (float)v;
            got++;
            line = e2;
        }

        if (got != (int)dim) {
            printf("维度不对：要 %d 个分量，给了 %d 个\n", dim, got);
            free(vec);
            return 1;
        }

        uint64_t rid = 0;
        db_err e = db_vec_insert(db, vec, (int32_t)label, &rid);
        free(vec);
        if (e != DB_OK)
            printf("插入失败: %s\n", db_strerror(e));
        else
            printf("已插入 row_id=%llu\n", (unsigned long long)rid);
        return 1;
    }

    if (strncmp(line, "KNN", 3) == 0) {
        line += 3;
        while (*line == ' ')
            line++;

        int exact = 0;
        if (strncmp(line, "EXACT", 5) == 0) {
            exact = 1;
            line += 5;
        }

        char *endp = NULL;
        long k = strtol(line, &endp, 10);
        if (endp == line || k <= 0) {
            printf("用法: VEC KNN [EXACT] <k> <v0> ... <vN>\n");
            return 1;
        }

        uint16_t dim = db_vec_dim(db);
        float *vec = (float *)calloc(dim, sizeof(float));
        if (!vec)
            return 1;

        int got = 0;
        line = endp;
        while (*line) {
            while (*line == ' ' || *line == ',')
                line++;
            if (!*line || *line == '\n' || *line == '\r')
                break;
            char *e2 = NULL;
            double v = strtod(line, &e2);
            if (e2 == line)
                break;
            if (got < dim)
                vec[got] = (float)v;
            got++;
            line = e2;
        }

        if (got != (int)dim) {
            printf("维度不对：要 %d 个分量，给了 %d 个\n", dim, got);
            free(vec);
            return 1;
        }

        db_vec_hit *hits = (db_vec_hit *)calloc((size_t)k, sizeof(db_vec_hit));
        if (!hits) {
            free(vec);
            return 1;
        }

        uint32_t n = 0;
        uint64_t reads0 = db_vec_page_reads(db);
        db_err e;
        if (exact)
            e = db_vec_knn_exact(db, vec, (uint32_t)k, hits, &n);
        else
            e = db_vec_knn(db, vec, (uint32_t)k, 0, hits, &n);
        uint64_t reads1 = db_vec_page_reads(db);

        free(vec);

        if (e != DB_OK) {
            printf("查询失败: %s\n", db_strerror(e));
        } else {
            for (uint32_t i = 0; i < n; i++)
                printf("  row_id=%llu  dist=%.6f\n",
                       (unsigned long long)hits[i].row_id, hits[i].dist);
            printf("(%u 个，读了 %llu 页)\n", n,
                   (unsigned long long)(reads1 - reads0));
        }
        free(hits);
        return 1;
    }

    if (strncmp(line, "STAT", 4) == 0) {
        if (!db_vec_exists(db)) {
            printf("还没有向量存储\n");
            return 1;
        }
        uint64_t n = 0;
        db_vec_count(db, &n);
        printf("%llu 个向量，维度 %u，累计读 %llu 页\n",
               (unsigned long long)n, db_vec_dim(db),
               (unsigned long long)db_vec_page_reads(db));
        return 1;
    }

    if (strncmp(line, "FLUSH", 5) == 0) {
        db_err e = db_vec_flush(db);
        if (e != DB_OK)
            printf("索引落盘失败: %s\n", db_strerror(e));
        else
            printf("索引已落盘\n");
        return 1;
    }

    printf("用法: VEC CREATE / INSERT / KNN / STAT / FLUSH\n");
    return 1;
}

