/*
 * db_result.c — 结果集访问器（include/db.h）
 *
 * 【为什么要拆出来】
 *
 * 硬规则：单文件不超过 800 行。db.c 一度到 888 行，
 * 里面混着三块互不相干的东西：开库（db_open）、关库（db_close 一族）、
 * 结果集访问器（这一块）。
 *
 * 结果集访问器是**纯读**的一组小函数 —— 只看 QueryResult 的几个字段，
 * 不碰实例、不碰锁、不碰 WAL。它天然可以单独成文件。
 *
 * 【这一块的行为约定】
 *
 * 所有访问器都对"空指针 / 越界"给**无害的默认值**，不报错：
 *   - 行数 / 列数 / 影响行数 → 0
 *   - 列名 / 单元格内容     → 空字符串（不是 NULL）
 *
 * 原因：调用方多半是"先看看有没有行"的写法，
 * 让它每次都判空会很啰嗦；而返回 NULL 会让 printf("%s") 之类的调用炸掉。
 */

#include "db_internal.h"

#include <stdlib.h>

int db_result_rows(const Result *r)
{
    return (r && r->qr) ? r->qr->nrows : 0;
}

int db_result_cols(const Result *r)
{
    return (r && r->qr) ? r->qr->ncols : 0;
}

int db_result_affected(const Result *r)
{
    return (r && r->qr) ? r->qr->affected : 0;
}

const char *db_result_colname(const Result *r, int col)
{
    if (!r || !r->qr || col < 0 || col >= r->qr->ncols)
        return "";
    return r->qr->colnames[col];
}

const char *db_result_value(const Result *r, int row, int col)
{
    if (!r || !r->qr || row < 0 || row >= r->qr->nrows)
        return "";
    if (col < 0 || col >= r->qr->ncols)
        return "";
    return r->qr->cells[row * r->qr->ncols + col];
}

void db_result_free(Result *r)
{
    if (!r)
        return;
    qr_free(r->qr);
    free(r);
}

const char *db_strerror(db_err e)
{
    switch (e) {
    case DB_OK:         return "OK";
    case DB_E_IO:       return "IO 错误";
    case DB_E_NOSPACE:  return "空间不足";
    case DB_E_CORRUPT:  return "数据损坏";
    case DB_E_PARSE:    return "语法错误";
    case DB_E_TXN:      return "事务错误";
    case DB_E_NOTFOUND: return "对象不存在";
    case DB_E_NOTSUP:   return "操作不支持";
    case DB_E_INVAL:    return "参数非法";
    default:            return "未知错误";
    }
}

const char *db_last_error(DB *db)
{
    return db ? db->errmsg : "";
}
