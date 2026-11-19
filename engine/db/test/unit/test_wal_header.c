/*
 * test_wal_header.c —— WAL 文件头与格式版本校验（W-D 的 D-1）
 *
 * ==================== 这个测试防的是什么 ====================
 *
 * wal_open 原来直接 `fopen(path, "r+b")`，**不校验文件格式**。
 * 后果：一个格式不兼容的 WAL（旧版本写的、或者根本不是 WAL）被打开后，
 * 恢复逻辑会**按自己的格式去解析它** ——
 * 最坏情况不是报错，而是**恢复出错误的数据**。
 *
 * 这是唯一一个「不做就可能静默损坏数据」的欠账，所以排 D-1。
 *
 * ==================== 判据 ====================
 *
 * 五种输入，五种结果 —— **每一种都必须有明确答案，不能「看着像就往下读」**：
 *
 *   ① 全新（0 字节）        ⇒ 成功，且写了一个合法的文件头
 *   ② 合法头 + 记录         ⇒ 成功，记录还能读出来
 *   ③ 未知格式版本          ⇒ E_CORRUPT（**明确拒绝**，不是当旧版兼容读）
 *   ④ 错误 magic            ⇒ E_CORRUPT
 *   ⑤ 截断的头（< 32 字节）  ⇒ E_CORRUPT
 *
 * ==================== 为什么「旧版本」也要报错 ====================
 *
 * 看起来「旧版本应该尽量兼容读」更友好，但这里做不到：
 *
 *   文件头之前的格式**没有任何版本标记**。所以一份「看起来像 v0」的文件，
 *   和一份「被改坏了的 v1」，在字节层面**无法区分**。
 *   「兼容 v0」只能靠猜 —— 而猜错的代价是恢复出错误的数据。
 *
 *   ⇒ 所以取「明确报错」。这是「宁可报错，不可静默错」的取舍。
 *   ⇒ 代价：升级内核后旧库需要重建（或写一次迁移工具）。这个代价写在
 *     wal.h 的注释里，不藏着。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define TRUNCATE_FILE(f, n) _chsize(_fileno(f), (n))
#else
#include <unistd.h>
#define TRUNCATE_FILE(f, n) ftruncate(fileno(f), (n))
#endif

#include "common/config.h"
#include "common/crc.h"
#include "common/error.h"
#include "wal/wal.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define HPATH "test/tmp/wal_hdr.wal"

/*
 * 手工造一个 WAL 文件：先写文件头（合法或按参数破坏），再写一条真记录。
 *
 * 不能直接调 wal_open + wal_append —— 那样用的是「被测代码」造夹具，
 * 测不出「外来文件被怎么对待」。
 */
static int make_wal(const char *path,
                    u32 magic, u32 ver, u32 page_size, u32 endian, u32 hdr_size,
                    int with_record)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    /* ---- 文件头：32 字节，与 WalFileHeader 逐字段对应 ---- */
    {
        u8 h[WAL_FILE_HDR_SIZE];
        memset(h, 0, sizeof(h));
        put_u32(h + 0,  magic);
        put_u32(h + 4,  ver);
        put_u32(h + 8,  page_size);
        put_u32(h + 12, endian);
        put_u32(h + 16, hdr_size);
        /* 20..27 是 reserved，必须为 0（已经是 0）*/
        fwrite(h, 1, sizeof(h), f);
    }

    /* ---- 可选：一条合法记录（28 字节头 + 载荷 + crc32）---- */
    if (with_record) {
        u8 hdr[WAL_HDR];
        u8 payload[4] = { 0xAB, 0xCD, 0xEF, 0x01 };
        memset(hdr, 0, sizeof(hdr));
        put_u32(hdr + 0, 1u);                /* lsn */
        put_u32(hdr + 4, 0u);                /* prev_lsn */
        put_u32(hdr + 8, 7u);                /* xid */
        hdr[12] = WAL_INSERT;
        put_u32(hdr + 16, 42u);              /* page_id */
        put_u16(hdr + 20, 3u);               /* slot */
        put_u16(hdr + 22, 0u);               /* blen */
        put_u16(hdr + 24, (u16)sizeof(payload));
        put_u16(hdr + 26, 0u);

        u32 crc = crc32_update(0xFFFFFFFFu, hdr, sizeof(hdr));
        crc = crc32_update(crc, payload, sizeof(payload));
        crc = ~crc;

        fwrite(hdr, 1, sizeof(hdr), f);
        fwrite(payload, 1, sizeof(payload), f);
        fwrite(&crc, 1, 4, f);
    }

    fclose(f);
    return 0;
}

/*
 * ==================== ① 全新文件：要自动写头 ====================
 *
 * 空文件 = 从来没有日志。这不是错误，是「第一次开库」。
 * 但**必须**在开完之后文件里有头 —— 否则下一次开库又会把它当成
 * 「截断的头」而报错（自相矛盾）。
 */
static void t1_fresh_writes_header(void)
{
    remove(HPATH);
    Wal *w = NULL;
    CHECK(wal_open(HPATH, &w) == OK);
    CHECK(w != NULL);
    if (w) {
        CHECK(wal_nrecords(w) == 0);
        CHECK(wal_last_lsn(w) == 0);
        wal_close(w);
    }

    /* 开完之后文件里必须已经有头（下次开库才不会被当成截断）*/
    {
        FILE *f = fopen(HPATH, "rb");
        CHECK(f != NULL);
        if (f) {
            long sz = 0;
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fclose(f);
            CHECK(sz >= (long)WAL_FILE_HDR_SIZE);
        }
    }

    /* 再开一次：这次走「有头」的分支，也要成功 */
    w = NULL;
    CHECK(wal_open(HPATH, &w) == OK);
    if (w)
        wal_close(w);
}

/*
 * ==================== ② 合法头 + 记录：能读出来 ====================
 *
 * 这是「加了头之后，正常路径没被搞坏」的证据。
 * 特别要验 **LSN 从 1 起算**（不是从「文件头占了多少字节」算）——
 * 若扫描路径没跳过文件头，第一条记录会被当成垃圾、LSN 会是 0。
 */
static void t2_valid_file_reads_record(void)
{
    remove(HPATH);
    make_wal(HPATH, WAL_MAGIC, WAL_FORMAT_VERSION, PAGE_SIZE, 0x01020304u,
             WAL_FILE_HDR_SIZE, 1);

    Wal *w = NULL;
    CHECK(wal_open(HPATH, &w) == OK);
    if (!w)
        return;

    CHECK(wal_nrecords(w) == 1);
    CHECK(wal_last_lsn(w) == 1);
    CHECK(wal_max_xid(w) == 7);

    WalRec r;
    CHECK(wal_scan_begin(w) == OK);
    CHECK(wal_scan_next(w, &r) == OK);
    CHECK(r.lsn == 1);
    CHECK(r.type == WAL_INSERT);
    CHECK(r.page_id == 42);
    CHECK(wal_scan_next(w, &r) == E_EOF);
    wal_scan_end(w);

    /* 追加一条：必须排在现有记录之后，LSN = 2 */
    u32 lsn = wal_append_xid(w, WAL_INSERT, 7, 43, 0, 0, NULL, 0,
                             (const u8 *)"y", 1);
    CHECK(lsn == 2);

    /* 追加完之后再扫：两条都在，文件头没被覆盖 */
    int n = 0;
    CHECK(wal_scan_begin(w) == OK);
    while (wal_scan_next(w, &r) == OK)
        n++;
    wal_scan_end(w);
    CHECK(n == 2);

    wal_close(w);
}

/*
 * ==================== ③ 未知格式版本：明确拒绝 ====================
 *
 * 「未来的版本写的日志」—— 当前内核不认识。**必须报错**，不能猜。
 */
static void t3_unknown_version(void)
{
    remove(HPATH);
    make_wal(HPATH, WAL_MAGIC, WAL_FORMAT_VERSION + 999u, PAGE_SIZE,
             0x01020304u, WAL_FILE_HDR_SIZE, 1);

    Wal *w = NULL;
    CHECK(wal_open(HPATH, &w) == E_CORRUPT);
    CHECK(w == NULL);
}

/*
 * ==================== ④ 错误 magic：不是 WAL ====================
 *
 * 指错了文件名（比如把数据文件当日志打开）—— 必须报错。
 */
static void t4_bad_magic(void)
{
    remove(HPATH);
    make_wal(HPATH, 0xDEADBEEFu, WAL_FORMAT_VERSION, PAGE_SIZE,
             0x01020304u, WAL_FILE_HDR_SIZE, 1);

    Wal *w = NULL;
    CHECK(wal_open(HPATH, &w) == E_CORRUPT);
    CHECK(w == NULL);
}

/*
 * ==================== ⑤ 截断的头：文件比头还短 ====================
 *
 * 写到一半断电 —— 头的后半截没了。
 * 注意与「0 字节」的区别：0 字节是全新库（要写头），
 * 10 字节是**半个头**（必须报错）。
 */
static void t5_truncated_header(void)
{
    remove(HPATH);
    {
        FILE *f = fopen(HPATH, "wb");
        CHECK(f != NULL);
        if (f) {
            u8 part[10];
            memset(part, 0, sizeof(part));
            put_u32(part + 0, WAL_MAGIC);   /* magic 对，但头不完整 */
            fwrite(part, 1, sizeof(part), f);
            fclose(f);
        }
    }

    Wal *w = NULL;
    CHECK(wal_open(HPATH, &w) == E_CORRUPT);
    CHECK(w == NULL);
}

/*
 * ==================== ⑥ page_size / endian 不匹配：也要拒绝 ====================
 *
 * 这两项比版本号更容易造成**静默损坏**：
 *   - page_size 变了 ⇒ 载荷里的页内偏移全错，读出来是别的字段；
 *   - 字节序反了   ⇒ 载荷每一个整数都错。
 * 两者都不会报错，只会读出垃圾 —— 所以必须在校验里拦住。
 */
static void t6_page_size_and_endian(void)
{
    remove(HPATH);
    make_wal(HPATH, WAL_MAGIC, WAL_FORMAT_VERSION, PAGE_SIZE * 2u,
             0x01020304u, WAL_FILE_HDR_SIZE, 1);
    Wal *w = NULL;
    CHECK(wal_open(HPATH, &w) == E_CORRUPT);
    CHECK(w == NULL);

    remove(HPATH);
    make_wal(HPATH, WAL_MAGIC, WAL_FORMAT_VERSION, PAGE_SIZE,
             0x04030201u, WAL_FILE_HDR_SIZE, 1);
    w = NULL;
    CHECK(wal_open(HPATH, &w) == E_CORRUPT);
    CHECK(w == NULL);
}

/* ==================== ⑦ wal_reset 之后：头必须回来 ====================
 *
 * wal_reset 用 "wb" 重开文件 —— 那会把头**一起截掉**。
 * 若不补写头，下次开库就变成「截断的头」⇒ 自伤。
 */
static void t7_reset_keeps_header(void)
{
    remove(HPATH);
    Wal *w = NULL;
    CHECK(wal_open(HPATH, &w) == OK);
    if (!w)
        return;

    wal_append_xid(w, WAL_INSERT, 1, 10, 0, 0, NULL, 0, (const u8 *)"z", 1);
    CHECK(wal_nrecords(w) == 1);

    CHECK(wal_reset(w) == OK);
    CHECK(wal_nrecords(w) == 0);
    CHECK(wal_last_lsn(w) == 0);
    wal_close(w);

    /* 关掉再开：reset 之后文件里必须仍有合法头 */
    w = NULL;
    CHECK(wal_open(HPATH, &w) == OK);
    if (w) {
        CHECK(wal_nrecords(w) == 0);
        CHECK(wal_last_lsn(w) == 0);
        wal_close(w);
    }
}

int main(void)
{
    printf("test_wal_header:\n");

    t1_fresh_writes_header();   printf("  [1] 全新文件自动写头\n");
    t2_valid_file_reads_record(); printf("  [2] 合法头 + 记录可读（LSN 从 1 起）\n");
    t3_unknown_version();       printf("  [3] 未知格式版本 ⇒ E_CORRUPT\n");
    t4_bad_magic();             printf("  [4] 错误 magic ⇒ E_CORRUPT\n");
    t5_truncated_header();      printf("  [5] 截断的头 ⇒ E_CORRUPT\n");
    t6_page_size_and_endian();  printf("  [6] page_size / endian 不匹配 ⇒ E_CORRUPT\n");
    t7_reset_keeps_header();    printf("  [7] wal_reset 之后头还在\n");

    remove(HPATH);

    if (failures == 0) {
        printf("  7 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
