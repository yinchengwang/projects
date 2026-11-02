#ifndef DB_COMMON_ERROR_H
#define DB_COMMON_ERROR_H

/*
 * 返回码 + goto fail。C 没有异常，这是最可控的模式。
 * （PG 用 setjmp/longjmp，性能好但调试痛苦；第一版不要学。）
 */

enum {
    OK = 0,
    E_NOSPACE,   /* 页/文件空间不足 */
    E_CORRUPT,   /* 校验失败、结构非法 */
    E_IO,        /* 系统调用失败 */
    E_NOTFOUND,  /* key / 页 / 表 / 行不存在 */
    E_TXN,       /* 事务状态非法 */
    E_INVAL,     /* 参数非法 */
    E_EOF,       /* 扫描结束（不是错误，但需要和"出错"区分） */
    E_NOTSUP     /* 操作不支持（能力协商：模态/索引没有这个能力） */
};

/*
 * 【E_NOTSUP 是能力协商的语言】（DESIGN.md §5.2）
 *
 * "这个结构没有这个能力"和"这个操作失败了"是两回事：
 *   - E_NOTSUP → 调用方**换一个做法**（比如退回全表扫描）
 *   - 其他错误 → 调用方**报错**
 *
 * 混用会让优化器没法判断"要不要换个计划"，
 * 于是要么把不支持当成失败（用户体验差），
 * 要么把失败当成不支持（静默走错路）。
 */

/* 用法：
 *   int rc = OK;
 *   TRY(pager_read(pg, id));
 *   TRY(page_insert(pg, rec, len));
 *   goto done;
 * fail:
 *   // rc 已带错误码
 * done:
 *   return rc;
 */
#define TRY(e) do { rc = (e); if (rc != OK) goto fail; } while (0)

const char *err_name(int rc);

#endif /* DB_COMMON_ERROR_H */
