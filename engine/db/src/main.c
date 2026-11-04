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
 * 窗口和 KNN 不走 SQL，与向量模态的 VEC KNN、全文模态的 FT SEARCH
 * 同一个判断：几何谓词和距离排序都算不出来。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "repl_ft.h"
#include "repl_geo.h"
#include "repl_vec.h"

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

/* ---------------- blob 命令 ---------------- */

/*
 * 处理一条 BLOB 命令。返回 true 表示这条命令被消费了。
 */
static int handle_blob(DB *db, char *line)
{
    /* 跳过 "BLOB" 关键字 */
    while (*line == ' ')
        line++;
    if (strncmp(line, "BLOB", 4) != 0)
        return 0;
    line += 4;

    while (*line == ' ')
        line++;

    if (strncmp(line, "CREATE", 6) == 0) {
        line += 6;
        unsigned long long nframes = strtoull(line, &line, 10);
        while (*line == ' ')
            line++;

        /*
         * 两种用法：
         *   BLOB CREATE 10              只建空对象
         *   BLOB CREATE 10 <内容>       建对象并直接写入内容
         *
         * 内容的长度决定需要几帧 —— 帧数不够就自动补齐。
         * 这是对象存储的常见用法（PUT 时先定大小）。
         */
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
            n--;

        if (nframes == 0 && n > 0) {
            /* 没给帧数就按内容长度算（一帧 4076 字节） */
            nframes = (n + 4075) / 4076;
        }
        if (nframes == 0) {
            fprintf(stderr, "错误: BLOB CREATE 需要帧数\n");
            return 1;
        }

        db_blob_id id = 0;
        db_err e = db_blob_create(db, nframes, 0, &id);
        if (e != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_strerror(e));
            return 1;
        }
        printf("blob %llu\n", (unsigned long long)id);

        if (n > 0) {
            e = db_blob_write(db, id, line, n);
            if (e != DB_OK)
                fprintf(stderr, "错误: %s\n", db_strerror(e));
            else
                printf("已写入 %zu 字节\n", n);
        }
        return 1;
    }

    if (strncmp(line, "WRITE", 5) == 0) {
        line += 5;
        unsigned long long id = strtoull(line, &line, 10);
        while (*line == ' ')
            line++;
        size_t n = strlen(line);
        /* 去掉行尾换行 */
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
            n--;

        db_err e = db_blob_write(db, (db_blob_id)id, line, n);
        if (e != DB_OK)
            fprintf(stderr, "错误: %s\n", db_strerror(e));
        else
            printf("已写入 %zu 字节\n", n);
        return 1;
    }

    if (strncmp(line, "READ", 4) == 0) {
        line += 4;
        unsigned long long id = strtoull(line, &line, 10);
        unsigned long long frame = strtoull(line, NULL, 10);

        unsigned char buf[8192];
        uint32_t n = 0;
        db_err e = db_blob_read_frame(db, (db_blob_id)id, frame, buf, sizeof(buf), &n);
        if (e != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_strerror(e));
            return 1;
        }
        printf("帧 %llu (%u 字节):\n", frame, n);
        fwrite(buf, 1, n, stdout);
        printf("\n");
        return 1;
    }

    if (strncmp(line, "STAT", 4) == 0) {
        line += 4;
        unsigned long long id = strtoull(line, NULL, 10);

        uint64_t nframes = 0, total = 0;
        uint32_t fsize = 0;
        db_err e = db_blob_stat(db, (db_blob_id)id, &nframes, &fsize, &total);
        if (e != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_strerror(e));
            return 1;
        }
        printf("blob %llu: %llu 帧 × %u 字节 = %llu 字节\n",
               id, (unsigned long long)nframes, fsize,
               (unsigned long long)total);
        return 1;
    }

    if (strncmp(line, "LIST", 4) == 0) {
        uint32_t n = db_blob_count(db);
        printf("%u 个对象:\n", n);
        for (uint32_t i = 0; i < n; i++) {
            db_blob_id id = db_blob_id_at(db, i);
            uint64_t nf = 0, total = 0;
            uint32_t fs = 0;
            if (db_blob_stat(db, id, &nf, &fs, &total) == DB_OK)
                printf("  blob %llu: %llu 帧, %llu 字节\n",
                       (unsigned long long)id, (unsigned long long)nf,
                       (unsigned long long)total);
        }
        return 1;
    }

    fprintf(stderr, "错误: 未知的 BLOB 子命令\n");
    return 1;
}

/* ---------------- graph 命令 ---------------- */

/*
 * GRAPH 命令。
 *
 *   GRAPH CREATE <列定义>   建图
 *   GRAPH SCHEMA            看属性 schema
 *   GRAPH NODES             列出所有节点
 *   GRAPH EDGES             列出所有边
 *   GRAPH HOP <rid>         跳一步（出邻居）
 *   GRAPH HOPIN <rid>       跳一步（入邻居）
 *   GRAPH STAT              统计
 *
 * 列定义语法：name:TYPE[(宽度)][!]
 *   TYPE  = INT4 / INT8 / BOOL / TEXT
 *   TEXT **必须**带宽度 —— 定长是"定长槽页"的前提，
 *         宽度不定的属性会让节点记录变长，整个设计就塌了。
 *   末尾的 ! 表示 NOT NULL（默认可空）。
 *
 *   例：GRAPH CREATE id:INT4!, name:TEXT(16), score:INT8
 *
 * 【为什么建图要走 REPL 命令而不是 SQL】
 *
 * CREATE TABLE 走的是 catalog（图模态碰不到 catalog —— 硬规则 6），
 * 而图自己的 schema 由图自己管。所以建图是"图模态的一个动作"，
 * 和 BLOB CREATE 同类。
 */

/* 大小写不敏感地比较 n 个字符（只看前缀，不看结尾） */
static int eq_ci_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb)
            return 0;
    }
    return 1;
}

/*
 * 解析列定义串。成功返回 0，失败返回 -1。
 *
 * 失败时**不猜**：宁可报错让用户改语法，也不要建出一个语义不对的图
 * （比如把 TEXT(16) 读成 TEXT 而宽度是 0 —— 那样插进去的文本会被截断）。
 */
static int parse_graph_cols(const char *spec, db_graph_col *cols, int max, int *n_out)
{
    int n = 0;
    const char *p = spec;

    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p)
            break;

        /* --- 列名 --- */
        const char *nstart = p;
        while (*p && *p != ':' && *p != ' ' && *p != ',' && *p != '\t' &&
               *p != '\r' && *p != '\n')
            p++;
        size_t nlen = (size_t)(p - nstart);
        if (nlen == 0 || nlen >= DB_GRAPH_NAME_MAX)
            return -1;
        if (*p != ':')
            return -1;
        p++;

        /* --- 类型 --- */
        const char *tstart = p;
        while (*p && *p != '(' && *p != ' ' && *p != ',' && *p != '!' &&
               *p != '\t' && *p != '\r' && *p != '\n')
            p++;
        size_t tlen = (size_t)(p - tstart);

        db_graph_col c;
        memset(&c, 0, sizeof(c));
        memcpy(c.name, nstart, nlen);
        c.name[nlen] = '\0';

        if (tlen == 4 && eq_ci_n(tstart, "INT4", tlen))
            c.type = DB_GRAPH_INT4;
        else if (tlen == 4 && eq_ci_n(tstart, "INT8", tlen))
            c.type = DB_GRAPH_INT8;
        else if (tlen == 4 && eq_ci_n(tstart, "BOOL", tlen))
            c.type = DB_GRAPH_BOOL;
        else if (tlen == 4 && eq_ci_n(tstart, "TEXT", tlen))
            c.type = DB_GRAPH_TEXT;
        else
            return -1;

        /* --- 宽度（只有 TEXT 需要） --- */
        if (*p == '(') {
            p++;
            char *endp = NULL;
            long w = strtol(p, &endp, 10);
            if (endp == p || *endp != ')' || w <= 0)
                return -1;
            p = endp + 1;
            c.width = (int)w;
        } else if (c.type == DB_GRAPH_TEXT) {
            return -1;      /* TEXT 没有宽度 = 不是定长 */
        }

        /* --- 可空标记 --- */
        c.nullable = 1;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '!') {
            c.nullable = 0;
            p++;
        }

        if (n >= max)
            return -1;
        cols[n++] = c;

        /* --- 分隔 --- */
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '\0' || *p == '\r' || *p == '\n')
            break;
        return -1;
    }

    *n_out = n;
    return 0;
}

/* 打印一个属性值 */
static void print_graph_value(const db_graph_value *v)
{
    if (v->is_null) {
        printf("NULL");
        return;
    }
    switch (v->type) {
    case DB_GRAPH_INT4: printf("%d", (int)v->i); break;
    case DB_GRAPH_INT8: printf("%lld", (long long)v->i); break;
    case DB_GRAPH_BOOL: printf("%s", v->i ? "true" : "false"); break;
    case DB_GRAPH_TEXT: printf("%s", v->s); break;
    }
}

/* 处理一条 GRAPH 命令。返回 true 表示这条命令被消费了。 */
static int handle_graph(DB *db, char *line)
{
    while (*line == ' ')
        line++;
    if (strncmp(line, "GRAPH", 5) != 0)
        return 0;
    line += 5;

    while (*line == ' ')
        line++;

    if (strncmp(line, "CREATE", 6) == 0) {
        line += 6;

        db_graph_col cols[16];
        int n = 0;
        if (parse_graph_cols(line, cols, 16, &n) != 0) {
            fprintf(stderr, "错误: 列定义语法不对（例：id:INT4!, name:TEXT(16)）\n");
            return 1;
        }

        db_err e = db_graph_create(db, cols, n);
        if (e != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_last_error(db)[0] ? db_last_error(db)
                                                               : db_strerror(e));
            return 1;
        }
        printf("图已创建（%d 个属性列）\n", n);
        return 1;
    }

    if (strncmp(line, "SCHEMA", 6) == 0) {
        if (!db_graph_exists(db)) {
            fprintf(stderr, "错误: 图还不存在\n");
            return 1;
        }
        int n = db_graph_col_count(db);
        db_graph_col cols[16];
        int got = 0;
        if (db_graph_schema(db, cols, 16, &got) != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_strerror(db_graph_schema(db, cols, 16, &got)));
            return 1;
        }
        for (int i = 0; i < got; i++) {
            const char *t = cols[i].type == DB_GRAPH_INT4 ? "INT4"
                          : cols[i].type == DB_GRAPH_INT8 ? "INT8"
                          : cols[i].type == DB_GRAPH_BOOL ? "BOOL" : "TEXT";
            printf("  %-16s %s", cols[i].name, t);
            if (cols[i].type == DB_GRAPH_TEXT)
                printf("(%d)", cols[i].width);
            printf("%s\n", cols[i].nullable ? "" : " NOT NULL");
        }
        (void)n;
        return 1;
    }

    if (strncmp(line, "NODES", 5) == 0) {
        db_graph_rid *ids = NULL;
        uint32_t n = 0;
        if (db_graph_list_nodes(db, &ids, &n) != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_last_error(db));
            return 1;
        }

        int ncols = db_graph_col_count(db);
        db_graph_col cols[16];
        int got = 0;
        db_graph_schema(db, cols, 16, &got);

        printf("%-20s", "node_id");
        for (int i = 0; i < got; i++)
            printf(" %-16s", cols[i].name);
        printf("\n");

        db_graph_value vals[16];
        for (uint32_t i = 0; i < n; i++) {
            printf("%-20llu", (unsigned long long)ids[i]);
            int nv = 0;
            if (db_graph_get_node(db, ids[i], vals, 16, &nv) == DB_OK) {
                for (int c = 0; c < nv; c++) {
                    printf(" ");
                    print_graph_value(&vals[c]);
                }
            }
            printf("\n");
        }
        printf("(%u 个节点)\n", n);
        db_graph_free(ids);
        (void)ncols;
        return 1;
    }

    if (strncmp(line, "EDGES", 5) == 0) {
        db_graph_rid *ids = NULL;
        uint32_t n = 0;
        if (db_graph_list_edges(db, &ids, &n) != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_last_error(db));
            return 1;
        }
        printf("%-20s %-20s %-20s\n", "edge_id", "src", "dst");
        for (uint32_t i = 0; i < n; i++) {
            db_graph_rid s = 0, d = 0;
            if (db_graph_get_edge(db, ids[i], &s, &d) == DB_OK)
                printf("%-20llu %-20llu %-20llu\n", (unsigned long long)ids[i],
                       (unsigned long long)s, (unsigned long long)d);
            else
                printf("%-20llu ?\n", (unsigned long long)ids[i]);
        }
        printf("(%u 条边)\n", n);
        db_graph_free(ids);
        return 1;
    }

    if (strncmp(line, "HOPIN", 5) == 0) {
        line += 5;
        unsigned long long rid = strtoull(line, NULL, 10);
        db_graph_rid *out = NULL;
        uint32_t n = 0;
        if (db_graph_hop_in(db, (db_graph_rid)rid, &out, &n) != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_last_error(db));
            return 1;
        }
        printf("入邻居 (%u 个):", n);
        for (uint32_t i = 0; i < n; i++)
            printf(" %llu", (unsigned long long)out[i]);
        printf("\n");
        db_graph_free(out);
        return 1;
    }

    if (strncmp(line, "HOP", 3) == 0) {
        line += 3;
        unsigned long long rid = strtoull(line, NULL, 10);
        db_graph_rid *out = NULL;
        uint32_t n = 0;
        if (db_graph_hop(db, (db_graph_rid)rid, &out, &n) != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_last_error(db));
            return 1;
        }
        printf("出邻居 (%u 个):", n);
        for (uint32_t i = 0; i < n; i++)
            printf(" %llu", (unsigned long long)out[i]);
        printf("\n");
        db_graph_free(out);
        return 1;
    }

    if (strncmp(line, "STAT", 4) == 0) {
        uint64_t nn = 0, ne = 0;
        if (db_graph_count(db, &nn, &ne) != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_last_error(db));
            return 1;
        }
        printf("%llu 个节点, %llu 条边\n", (unsigned long long)nn,
               (unsigned long long)ne);
        return 1;
    }

    fprintf(stderr, "错误: 未知的 GRAPH 子命令\n");
    return 1;
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
            if (strncmp(p, "BLOB", 4) == 0) {
                handle_blob(db, line);
                len = 0;
                sql[0] = '\0';
                continue;
            }
            if (strncmp(p, "GRAPH", 5) == 0) {
                handle_graph(db, line);
                len = 0;
                sql[0] = '\0';
                continue;
            }
            if (strncmp(p, "VEC", 3) == 0) {
                handle_vec(db, line);
                len = 0;
                sql[0] = '\0';
                continue;
            }
            if (strncmp(p, "FT", 2) == 0) {
                handle_ft(db, line);
                len = 0;
                sql[0] = '\0';
                continue;
            }
            if (strncmp(p, "GEO", 3) == 0) {
                handle_geo(db, line);
                len = 0;
                sql[0] = '\0';
                continue;
            }
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
