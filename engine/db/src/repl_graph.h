#ifndef DB_REPL_GRAPH_H
#define DB_REPL_GRAPH_H

#include "db.h"

/*
 * graph 模态的 REPL 入口。
 *
 * 返回 1 表示这一行被消费了（是 GRAPH 命令），0 表示不是（交回主循环当 SQL）。
 *
 * 【为什么建图与遍历走命令而不是 SQL】
 * 建图要用图自己的 schema（图碰不到 catalog —— 硬规则 6）；
 * 遍历是递归（"3 跳"），SQL 表达不了。标量查询仍然走 SQL 虚拟表。
 */
int handle_graph(DB *db, char *line);

#endif /* DB_REPL_GRAPH_H */
