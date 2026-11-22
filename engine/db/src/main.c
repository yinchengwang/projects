/*
 * main.c — 命令行 REPL
 *
 * 用法：
 *   ./db.exe [文件]        交互模式
 *   ./db.exe 文件 < x.sql  批处理
 *
 * 【blob 命令】
 *
 * blob 模态是独立的一层，有自己的 API（不是 SQL 语句）。
 * 所以 REPL 里给它单独的命令前缀：
 *
 *   BLOB CREATE <nframes>      创建对象，打印 id
 *   BLOB WRITE <id> <text>     写入文本内容（对象不可变，写完定稿）
 *   BLOB READ <id> <frame>     读第 N 帧
 *   BLOB STAT <id>             对象元信息
 *   BLOB LIST                  列出所有对象
 *
 * 为什么不加成 SQL 语句（CREATE BLOB / SELECT FROM BLOB）：
 * 因为 blob 模态还没接入 SQL 层（没有 SPI 的执行器接线）。
 * 模态之间不许互相依赖 —— 加了 SQL 语句就要让 query 层认识 blob，
 * 那是 v0.9 SPI 接线时该做的事。现在先给一个直接可用的命令入口。
 *
 * 【graph 命令】
 *
 * 图模态已经有 SPI 接线了（graph_nodes / graph_edges 两张虚拟表），
 * 所以标量查询直接写 SQL 就行。但**建图**和**遍历**还得走命令：
 *
 *   GRAPH CREATE <列定义>   建图（schema 是图自己的事，不进 catalog）
 *   GRAPH SCHEMA            看属性 schema
 *   GRAPH NODES / EDGES     列出所有节点 / 边
 *   GRAPH HOP <rid>         跳一步（出邻居）
 *   GRAPH HOPIN <rid>       跳一步（入邻居）
 *   GRAPH STAT              统计
 *
 * 遍历走命令而不是 SQL，是因为 SQL 表达不了递归（"3 跳"）。
 *
 * 【geo 命令】
 *
 * 空间模态的元数据走 SPI 虚拟表（geo_points），
 * 但**建存储**和**空间谓词查询**还得走命令：
 *
 *   GEO CREATE              建几何存储（一个库只能建一次）
 *   GEO EXISTS              这个库有没有几何存储
 *   GEO INSERT <x> <y>      插一个点
 *   GEO WINDOW <minx> <miny> <maxx> <maxy>   窗口查询
 *   GEO KNN <x> <y> <k>     近邻查询
 *   GEO DELETE <row_id>     删一个点
 *   GEO COUNT               活着的点数
 *   GEO REINDEX             重建索引
 *   GEO READS               诊断：累计读了多少页
 *
 * 【blob 与 graph 的实现在别处】
 *
 * 这两个模态的 REPL 命令不在本文件里 —— blob 见 `repl_blob.c`，
 * graph 见 `repl_graph.c`。本文件只保留派发分支（按命令前缀转交）。
 * 其余五个模态（vec / ft / geo / ts）一直就是独立的 `repl_<m>.c`。
 *
 * 窗口和 KNN 不走 SQL，与向量模态的 VEC KNN、全文模态的 FT SEARCH
 * 同一个判断：几何谓词和距离排序都算不出来。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#if DB_HAS_BLOB
#include "repl_blob.h"
#endif
#if DB_HAS_GRAPH
#include "repl_graph.h"
#endif
#if DB_HAS_FT
#include "repl_ft.h"
#endif
#if DB_HAS_GEO
#include "repl_geo.h"
#endif
#if DB_HAS_TS
#include "repl_ts.h"
#endif
#if DB_HAS_VECTOR
#include "repl_vec.h"
#endif

static void print_result(Result *r)
{
    int ncols = db_result_cols(r);
    int nrows = db_result_rows(r);

    if (ncols == 0) {
        printf("OK, %d 行受影响\n", db_result_affected(r));
        return;
    }

    /* 每列宽度 */
    int w[32];
    if (ncols > 32)
        ncols = 32;
    for (int c = 0; c < ncols; c++) {
        w[c] = (int)strlen(db_result_colname(r, c));
        for (int i = 0; i < nrows; i++) {
            int n = (int)strlen(db_result_value(r, i, c));
            if (n > w[c])
                w[c] = n;
        }
    }

    for (int c = 0; c < ncols; c++)
        printf("%-*s  ", w[c], db_result_colname(r, c));
    printf("\n");
    for (int c = 0; c < ncols; c++) {
        for (int k = 0; k < w[c]; k++)
            putchar('-');
        printf("  ");
    }
    printf("\n");

    for (int i = 0; i < nrows; i++) {
        for (int c = 0; c < ncols; c++)
            printf("%-*s  ", w[c], db_result_value(r, i, c));
        printf("\n");
    }
    printf("(%d 行)\n", nrows);
}


int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "test.db";

    DB *db = NULL;
    db_err e = db_open(path, &db);
    if (e != DB_OK) {
        fprintf(stderr, "打开失败: %s\n", db_strerror(e));
        return 1;
    }

    static char sql[16384];
    size_t len = 0;
    char line[2048];
    int prompt = (argc <= 1);

    if (!prompt)
        printf("db: %s\n", path);

    for (;;) {
        if (prompt) {
            printf(len ? "  -> " : "db> ");
            fflush(stdout);
        }

        if (!fgets(line, sizeof(line), stdin))
            break;

        /*
         * BLOB / GRAPH 命令不走 SQL —— 它们是独立模态的 API。
         *
         * 必须在拼进 sql 缓冲**之前**判断，
         * 否则 "BLOB CREATE 10;" 会被当成 SQL 送去解析 → 语法错误。
         */
        {
            const char *p = line;
            while (*p == ' ')
                p++;
#if DB_HAS_BLOB
            if (strncmp(p, "BLOB", 4) == 0) {
                handle_blob(db, line);
                len = 0;
                sql[0] = '\0';
                continue;
            }
#endif
#if DB_HAS_GRAPH
            if (strncmp(p, "GRAPH", 5) == 0) {
                handle_graph(db, line);
                len = 0;
                sql[0] = '\0';
                continue;
            }
#endif
#if DB_HAS_VECTOR
            if (strncmp(p, "VEC", 3) == 0) {
                handle_vec(db, line);
                len = 0;
                sql[0] = '\0';
                continue;
            }
#endif
#if DB_HAS_FT
            if (strncmp(p, "FT", 2) == 0) {
                handle_ft(db, line);
                len = 0;
                sql[0] = '\0';
                continue;
            }
#endif
#if DB_HAS_GEO
            if (strncmp(p, "GEO", 3) == 0) {
                handle_geo(db, line);
                len = 0;
                sql[0] = '\0';
                continue;
            }
#endif
            /*
             * TS 命令同理（v1.5 步骤 5）。
             *
             * ⚠️ 必须排在 SQL 缓冲**之前**（坑 M6-59）：
             * 否则 "TS INSERT 100 1 2.5" 会被当成 SQL 送去解析 → 语法错误。
             */
#if DB_HAS_TS
            if (strncmp(p, "TS", 2) == 0) {
                handle_ts(db, line);
                len = 0;
                sql[0] = '\0';
                continue;
            }
#endif
        }

        size_t n = strlen(line);
        if (len + n + 1 >= sizeof(sql))
            len = 0;
        memcpy(sql + len, line, n + 1);
        len += n;

        if (!strchr(sql, ';'))
            continue;

        Result *r = NULL;
        e = db_exec(db, sql, &r);
        if (e != DB_OK) {
            /*
             * 有详细原因就报详细原因。
             *
             * 比如"表 blob_frames 不支持插入"比"操作不支持"有用得多 ——
             * 后者只说类别，前者说清楚是哪个表、哪个操作。
             */
            const char *detail = db_last_error(db);
            if (detail && detail[0])
                fprintf(stderr, "错误: %s\n", detail);
            else
                fprintf(stderr, "错误: %s\n", db_strerror(e));
        } else {
            print_result(r);
            db_result_free(r);
        }
        len = 0;
        sql[0] = '\0';
    }

    db_close(db);
    return 0;
}
