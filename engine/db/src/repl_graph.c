/*
 * repl_graph.c —— graph 模态的 REPL 命令（从 main.c 搬出）
 *
 * 【搬运纪律】除「去掉 static」与「调整 include」外，本文件与 main.c 里的
 * 原实现**逐字一致** —— 注释、错误处理、返回值语义全部原样。
 * 这是搬家，不是重写。
 */

#include "repl_graph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#if DB_HAS_GRAPH
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
#endif

/* 处理一条 GRAPH 命令。返回 true 表示这条命令被消费了。 */
#if DB_HAS_GRAPH
int handle_graph(DB *db, char *line)
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
#endif
