/*
 * smoke_blob.c —— blob 模态的链接期独立证明
 *
 * 链接集合 = 底座 + relational + blob。链接能过 ⇒ blob 没引用别的模态。
 *
 * 五步：建对象 → 写一条 → 读回 → 断言内容一致 → 关闭。
 * ⚠️ blob 的写**不可变**：db_blob_write 写完即定稿，要一次写够所有帧。
 */
#include <stdio.h>
#include <string.h>

#include "db.h"

#define FAIL(tag, ...)                                                         \
    do {                                                                       \
        fprintf(stderr, "smoke_blob: %s: ", (tag));                            \
        fprintf(stderr, __VA_ARGS__);                                          \
        fputc('\n', stderr);                                                   \
        return 1;                                                              \
    } while (0)

#define FRAME_SIZE 64u
#define NFRAMES    4u

int main(void)
{
    DB         *db = NULL;
    db_blob_id  id = 0;
    db_err      e;
    char        src[FRAME_SIZE * NFRAMES];
    char        dst[FRAME_SIZE];
    uint32_t    len_out = 0;
    uint64_t    nframes = 0, total_len = 0;
    uint32_t    frame_size = 0;

    remove("test/tmp/smoke_blob.db");
    remove("test/tmp/smoke_blob.db-wal");
    remove("test/tmp/smoke_blob.db-clog");

    e = db_open("test/tmp/smoke_blob.db", &db);
    if (e != DB_OK)
        FAIL("db_open", "%s", db_strerror(e));

    /* 建一个 4 帧 × 64 字节的对象 */
    e = db_blob_create(db, NFRAMES, FRAME_SIZE, &id);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_blob_create", "%s", db_last_error(db));
    }

    /* 写满全部帧 —— blob 是不可变的，一次写定稿 */
    for (uint32_t i = 0; i < FRAME_SIZE * NFRAMES; i++)
        src[i] = (char)('A' + (int)(i % 26));

    e = db_blob_write(db, id, src, sizeof(src));
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_blob_write", "%s", db_last_error(db));
    }

    /* 读回第 2 帧，和写进去的同一段比对 */
    memset(dst, 0, sizeof(dst));
    e = db_blob_read_frame(db, id, 2, dst, sizeof(dst), &len_out);    if (e != DB_OK) {
        db_close(db);
        FAIL("db_blob_read_frame", "%s", db_last_error(db));
    }
    if (len_out != FRAME_SIZE)
        FAIL("帧长度", "期望 %u，实际 %llu", FRAME_SIZE, (unsigned long long)len_out);
    if (memcmp(dst, src + 2 * FRAME_SIZE, FRAME_SIZE) != 0)
        FAIL("帧内容", "读回的内容与写入的不一致");

    /* 元信息也得对得上 */
    e = db_blob_stat(db, id, &nframes, &frame_size, &total_len);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_blob_stat", "%s", db_last_error(db));
    }
    if (nframes != NFRAMES || frame_size != FRAME_SIZE)
        FAIL("元信息", "期望 %u 帧 × %u 字节，实际 %llu 帧 × %u 字节",
             NFRAMES, FRAME_SIZE, (unsigned long long)nframes, frame_size);

    printf("smoke_blob: OK\n");
    db_close(db);
    return 0;
}
