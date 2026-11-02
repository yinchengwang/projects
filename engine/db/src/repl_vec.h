#ifndef DB_REPL_VEC_H
#define DB_REPL_VEC_H

#include "db.h"

/*
 * REPL 的 VEC 命令（v1.0 步骤 5）。
 *
 * 【为什么单独一个文件】
 *
 * main.c 是 REPL 的入口，本来只有 BLOB / GRAPH 两组命令。
 * 加上 VEC 之后它到了 833 行 —— 超过本项目 800 行的红线。
 *
 * 拆分的边界按"一组命令一个文件"：
 *   - BLOB 命令留在 main.c（它是第一个模态，和 REPL 骨架一起长大的）
 *   - GRAPH 命令留在 main.c（同上）
 *   - VEC 命令独立成文件
 *
 * 拆分的判据不是"行数快满了就拆"，而是"这块东西有自己的内聚性"：
 * VEC 命令要解析浮点向量，那套解析逻辑和 REPL 的 SQL 缓冲没有半点关系。
 */

int handle_vec(DB *db, char *line);

#endif /* DB_REPL_VEC_H */
