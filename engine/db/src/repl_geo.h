#ifndef DB_REPL_GEO_H
#define DB_REPL_GEO_H

#include <stdio.h>

#include "db.h"

/*
 * REPL 的 GEO 命令（v1.4 步骤 5）。
 *
 * 【为什么单独一个文件】
 *
 * 同一份理由见 repl_ft.h / repl_vec.h：一组命令一个文件。
 * GEO 的参数全是浮点数（坐标、矩形边界），解析方式与 SQL 缓冲、
 * 与 FT 的自由文本正文都不一样 —— 内聚性上就该分开。
 *
 * 【命令】
 *
 *   GEO CREATE              建几何存储（一个库只能建一次）
 *   GEO EXISTS              这个库有没有几何存储
 *   GEO INSERT <x> <y>      插一个点，打印分配到的 row_id
 *   GEO WINDOW <minx> <miny> <maxx> <maxy>
 *                           窗口查询，打印命中的 row_id
 *   GEO KNN <x> <y> <k>     近邻查询，打印 row_id + 平方距离
 *   GEO DELETE <row_id>     删一个点
 *   GEO COUNT               活着的点数
 *   GEO REINDEX             重建索引（清掉死条目）
 *   GEO READS               诊断：累计读了多少页
 *
 * 【为什么窗口 / KNN 不走 SQL】
 *
 * 与 VEC KNN / FT SEARCH 同一个判断（PLAN-v1.4 §1.1 明确把空间谓词
 * 划在标量谓词系统之外）：窗口是几何谓词，KNN 是距离排序，
 * 执行器都算不出来。
 *
 * 【元数据走 SQL】
 *
 *   SELECT row_id, x, y, deleted FROM geo_points;
 *
 * 那条路走的是 SPI 虚拟表，不用命令。
 */

int handle_geo(DB *db, char *line);

/*
 * 测试钩子：把命令输出重定向到指定流。
 *
 * 【为什么需要】
 *
 * REPL 的产物就是**给人看的输出** —— 只验 API 的话，
 * 命令层把 row_id 打错了照样测不出来。
 *
 * 不用 freopen 抓 stdout 的原因：MinGW 下 stdout 是个宏
 * （`(&__iob_func()[1])`），不是可赋值的左值，
 * 而且 freopen 会把原 stdout 弄丢、没法还原。
 * 这里留一个可还原的输出流指针，测试里指向临时文件即可。
 *
 * 传 NULL 恢复成 stdout（默认值）。返回被替换掉的那个流。
 */
FILE *repl_geo_set_output(FILE *f);

#endif /* DB_REPL_GEO_H */
