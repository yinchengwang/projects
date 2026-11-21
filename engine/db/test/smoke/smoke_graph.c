/*
 * smoke_graph.c —— 图模态的链接期独立证明
 *
 * 链接集合 = 底座 + relational + graph。链接能过 ⇒ graph 没引用别的模态。
 *
 * 五步：建图 → 加节点 → 加边 → 读回节点属性 → 断言内容 → 关闭。
 *
 * ⚠️ 列名必须是**非空**的：db_graph_create 会把空列名当非法 schema。
 */
#include <stdio.h>
#include <string.h>

#include "db.h"

#define FAIL(tag, ...)                                                         \
    do {                                                                       \
        fprintf(stderr, "smoke_graph: %s: ", (tag));                           \
        fprintf(stderr, __VA_ARGS__);                                          \
        fputc('\n', stderr);                                                   \
        return 1;                                                              \
    } while (0)

int main(void)
{
    DB            *db = NULL;
    db_graph_col   cols[2];
    db_graph_value props[2];
    db_graph_value back[2];
    db_graph_rid   n1 = 0, n2 = 0, edge = 0;
    db_err         e;
    int            n_out = 0;

    remove("test/tmp/smoke_graph.db");
    remove("test/tmp/smoke_graph.db-wal");
    remove("test/tmp/smoke_graph.db-clog");

    e = db_open("test/tmp/smoke_graph.db", &db);
    if (e != DB_OK)
        FAIL("db_open", "%s", db_strerror(e));

    /* 两列：id(INT4 非空) + name(TEXT 定长 16) */
    memset(cols, 0, sizeof(cols));
    snprintf(cols[0].name, sizeof(cols[0].name), "id");
    cols[0].type = DB_GRAPH_INT4;
    cols[0].nullable = 0;

    snprintf(cols[1].name, sizeof(cols[1].name), "name");
    cols[1].type = DB_GRAPH_TEXT;
    cols[1].width = 16;
    cols[1].nullable = 0;

    e = db_graph_create(db, cols, 2);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_graph_create", "%s", db_last_error(db));
    }

    /* 加两个节点 */
    memset(props, 0, sizeof(props));
    props[0].type = DB_GRAPH_INT4;
    props[0].i = 1;
    props[1].type = DB_GRAPH_TEXT;
    snprintf(props[1].s, sizeof(props[1].s), "alice");

    e = db_graph_add_node(db, props, 2, &n1);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_graph_add_node(1)", "%s", db_last_error(db));
    }

    props[0].i = 2;
    snprintf(props[1].s, sizeof(props[1].s), "bob");
    e = db_graph_add_node(db, props, 2, &n2);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_graph_add_node(2)", "%s", db_last_error(db));
    }

    /* 加一条 n1 → n2 的边 */
    e = db_graph_add_edge(db, n1, n2, &edge);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_graph_add_edge", "%s", db_last_error(db));
    }

    /* 读回节点 1 的属性并断言 */
    memset(back, 0, sizeof(back));
    e = db_graph_get_node(db, n1, back, 2, &n_out);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_graph_get_node", "%s", db_last_error(db));
    }
    if (n_out != 2)
        FAIL("属性个数", "期望 2，实际 %d", n_out);
    if (back[0].i != 1)
        FAIL("id 读回", "期望 1，实际 %lld", (long long)back[0].i);
    if (strcmp(back[1].s, "alice") != 0)
        FAIL("name 读回", "期望 alice，实际 %s", back[1].s);

    printf("smoke_graph: OK\n");
    db_close(db);
    return 0;
}
