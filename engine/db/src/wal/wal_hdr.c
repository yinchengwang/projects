#include "wal/wal_hdr.h"

#include <stdio.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "storage/page.h"
#include "wal/wal.h"

/*
 * wal_hdr.c —— WAL 文件头的读写与校验（v1.3 欠账 6 / W-D 的 D-1）
 *
 * 契约与「为什么要有这一层」见 wal_hdr.h。
 * 这里只放实现，以及三个必须写进代码里的取舍。
 */

/*
 * FSEEK / FTELL —— 与 wal.c 同一套宏。
 *
 * ⚠️ 两边必须一致：MinGW 下 4GB 以上文件要用 64 位偏移的 _fseeki64，
 * POSIX 用 fseeko。若这里用 32 位的 fseek，日志超过 4GB 时偏移会回绕
 * —— 而且不会报错，只会读到错的位置。
 */
#ifdef _WIN32
#define FSEEK _fseeki64
#define FTELL _ftelli64
#else
#define FSEEK fseeko
#define FTELL ftello
#endif

int wal_hdr_check(const u8 *h)
{
    if (h == NULL)
        return E_INVAL;

    /*
     * 每个字段都必须查，不能只查 magic。
     *
     * 见 wal.h 里「为什么 page_size 与 endian_marker 也要校验」那段：
     * 这两项搞错不会报错，只会读出垃圾 —— 比版本号更容易造成静默损坏。
     */
    if (get_u32(h + 0) != WAL_MAGIC)
        return E_CORRUPT;
    if (get_u32(h + 4) != WAL_FORMAT_VERSION)
        return E_CORRUPT;
    if (get_u32(h + 8) != PAGE_SIZE)
        return E_CORRUPT;
    if (get_u32(h + 12) != WAL_ENDIAN_MARKER)
        return E_CORRUPT;
    if (get_u32(h + 16) != WAL_FILE_HDR_SIZE)
        return E_CORRUPT;
    return OK;
}

int wal_hdr_write(FILE *f)
{
    u8 h[WAL_FILE_HDR_SIZE];

    if (f == NULL)
        return E_INVAL;

    memset(h, 0, sizeof(h));
    put_u32(h + 0,  WAL_MAGIC);
    put_u32(h + 4,  WAL_FORMAT_VERSION);
    put_u32(h + 8,  PAGE_SIZE);
    put_u32(h + 12, WAL_ENDIAN_MARKER);
    put_u32(h + 16, WAL_FILE_HDR_SIZE);
    /* 20..27 是 reserved，必须为 0（memset 已经是 0）*/

    if (FSEEK(f, 0, SEEK_SET) != 0)
        return E_IO;
    if (fwrite(h, 1, sizeof(h), f) != sizeof(h))
        return E_IO;
    return OK;
}

int wal_hdr_load(FILE *f, u64 *size_out)
{
    u8 h[WAL_FILE_HDR_SIZE];
    u64 sz = 0;
    int rc;

    if (f == NULL || size_out == NULL)
        return E_INVAL;

    if (FSEEK(f, 0, SEEK_END) == 0) {
        long long p = FTELL(f);
        if (p > 0)
            sz = (u64)p;
    }

    if (sz == 0) {
        /*
         * 空文件 = 全新日志（或者被 wal_reset / 上一个会话清空过）。
         *
         * 这不是错误，要**写一个头** —— 否则下次开库会把它当成
         * 「截断的头」而报错（自相矛盾）。
         */
        rc = wal_hdr_write(f);
        if (rc != OK)
            return rc;
        *size_out = WAL_FILE_HDR_SIZE;
        return OK;
    }

    /*
     * 非空文件：必须**至少**装得下一个头。
     *
     * ⚠️ 这与「空文件」是两个不同的分支，不能合并：
     *   0 字节        ⇒ 全新库，写头
     *   1..31 字节    ⇒ 写到一半断电，**半个头** ⇒ 报错
     */
    if (sz < WAL_FILE_HDR_SIZE)
        return E_CORRUPT;

    if (FSEEK(f, 0, SEEK_SET) != 0 ||
        fread(h, 1, sizeof(h), f) != sizeof(h))
        return E_IO;

    rc = wal_hdr_check(h);
    if (rc != OK)
        return rc;

    *size_out = sz;
    return OK;
}
