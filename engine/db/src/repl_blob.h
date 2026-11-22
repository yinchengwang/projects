#ifndef DB_REPL_BLOB_H
#define DB_REPL_BLOB_H

#include "db.h"

/*
 * blob 模态的 REPL 入口。
 *
 * 返回 1 表示这一行被消费了（是 BLOB 命令），0 表示不是（交回主循环当 SQL）。
 *
 * 【为什么它是独立文件】
 * 八模态的契约面是四件套，其中 ③ 是「REPL 族 = `src/repl_<m>.c`」。
 * blob 的 REPL 原本塞在 `main.c` 里（`static handle_blob`），
 * 与其余五个模态（vec / ft / geo / ts / graph）不对称。
 */
int handle_blob(DB *db, char *line);

#endif /* DB_REPL_BLOB_H */
