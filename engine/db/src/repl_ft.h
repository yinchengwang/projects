#ifndef DB_REPL_FT_H
#define DB_REPL_FT_H

#include "db.h"

/*
 * REPL 的 FT 命令（v1.3 步骤 4）。
 *
 * 【为什么单独一个文件】
 *
 * 同一份理由见 repl_vec.h：一组命令一个文件。
 * FT 命令要解析"文档正文"（可能带空格），那套解析和 REPL 的 SQL 缓冲
 * 没有半点关系 —— 内聚性上就该分开。
 *
 * 【命令】
 *
 *   FT INDEX <doc_id> <text>    灌一篇文档进全文索引
 *   FT SEARCH <query> <top_k>   检索，打印 doc_id + 分数
 */

int handle_ft(DB *db, char *line);

#endif /* DB_REPL_FT_H */
