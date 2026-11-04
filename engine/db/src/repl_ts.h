#ifndef DB_REPL_TS_H
#define DB_REPL_TS_H

#include <stdio.h>

#include "db.h"

/*
 * REPL 的 TS 命令（v1.5 步骤 5）。
 *
 * 【为什么单独一个文件】
 *
 * 与 repl_geo.h / repl_vec.h / repl_ft.h 同一个理由：
 * 一组命令一个文件，内聚性上就该分开。
 * TS 的参数是"毫秒时间戳 + 标签 + 读数"，解析方式与 SQL 缓冲、
 * 与 GEO 的浮点参数都不一样。
 *
 * 【命令】
 *
 *   TS CREATE                     建时序存储（一个库只能建一次）
 *   TS EXISTS                     这个库有没有时序存储
 *   TS INSERT <time_ms> <tag> <value>
 *                                 追加一条读数（时间戳按毫秒）
 *   TS COUNT                      读数总数
 *   TS PARTS                      列出分区（时间窗 / 状态 / 行数 / 页数）
 *   TS RETENTION <seconds>        TTL：回收过期分区
 *   TS READS                      诊断：累计读了多少页
 *
 * 【为什么 TS PARTS 走 SQL】
 *
 * 分区元数据是**视图**（ts_partitions 虚拟表），不是专用 API 的返回值 ——
 * 公开 API 里只有"分区个数"，没有"分区列表"，
 * 因为逐个分区取信息是内部调用，暴露出来就得承诺内存生命周期。
 * REPL 这层直接查视图：
 *
 *   SELECT start_ms, end_ms, state, n_rows, page_count FROM ts_partitions;
 *
 * 顺带也证明了虚拟表这条路是通的（与 GEO 的 geo_points 视图同一个口径）。
 */

int handle_ts(DB *db, char *line);

/*
 * 测试钩子：把命令输出重定向到指定流。
 *
 * 理由与 repl_geo_set_output 完全一样：REPL 的产物就是给人看的输出，
 * 只验 API 的话，命令层打错了照样测不出来。
 * 不用 freopen 抓 stdout 的原因：MinGW 下 stdout 是宏不是可赋值左值，
 * 而且 freopen 会把原 stdout 弄丢、没法还原。
 *
 * 传 NULL 恢复成 stdout（默认值）。返回被替换掉的那个流。
 */
FILE *repl_ts_set_output(FILE *f);

#endif /* DB_REPL_TS_H */
