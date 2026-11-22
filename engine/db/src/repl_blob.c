/*
 * repl_blob.c —— blob 模态的 REPL 命令（从 main.c 搬出）
 *
 * 【搬运纪律】除「去掉 static」与「调整 include」外，本文件与 main.c 里的
 * 原实现**逐字一致** —— 注释、错误处理、返回值语义全部原样。
 * 这是搬家，不是重写。
 */

#include "repl_blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- blob 命令 ---------------- */

/*
 * 处理一条 BLOB 命令。返回 true 表示这条命令被消费了。
 */
#if DB_HAS_BLOB
int handle_blob(DB *db, char *line)
{
    /* 跳过 "BLOB" 关键字 */
    while (*line == ' ')
        line++;
    if (strncmp(line, "BLOB", 4) != 0)
        return 0;
    line += 4;

    while (*line == ' ')
        line++;

    if (strncmp(line, "CREATE", 6) == 0) {
        line += 6;
        unsigned long long nframes = strtoull(line, &line, 10);
        while (*line == ' ')
            line++;

        /*
         * 两种用法：
         *   BLOB CREATE 10              只建空对象
         *   BLOB CREATE 10 <内容>       建对象并直接写入内容
         *
         * 内容的长度决定需要几帧 —— 帧数不够就自动补齐。
         * 这是对象存储的常见用法（PUT 时先定大小）。
         */
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
            n--;

        if (nframes == 0 && n > 0) {
            /* 没给帧数就按内容长度算（一帧 4076 字节） */
            nframes = (n + 4075) / 4076;
        }
        if (nframes == 0) {
            fprintf(stderr, "错误: BLOB CREATE 需要帧数\n");
            return 1;
        }

        db_blob_id id = 0;
        db_err e = db_blob_create(db, nframes, 0, &id);
        if (e != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_strerror(e));
            return 1;
        }
        printf("blob %llu\n", (unsigned long long)id);

        if (n > 0) {
            e = db_blob_write(db, id, line, n);
            if (e != DB_OK)
                fprintf(stderr, "错误: %s\n", db_strerror(e));
            else
                printf("已写入 %zu 字节\n", n);
        }
        return 1;
    }

    if (strncmp(line, "WRITE", 5) == 0) {
        line += 5;
        unsigned long long id = strtoull(line, &line, 10);
        while (*line == ' ')
            line++;
        size_t n = strlen(line);
        /* 去掉行尾换行 */
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
            n--;

        db_err e = db_blob_write(db, (db_blob_id)id, line, n);
        if (e != DB_OK)
            fprintf(stderr, "错误: %s\n", db_strerror(e));
        else
            printf("已写入 %zu 字节\n", n);
        return 1;
    }

    if (strncmp(line, "READ", 4) == 0) {
        line += 4;
        unsigned long long id = strtoull(line, &line, 10);
        unsigned long long frame = strtoull(line, NULL, 10);

        unsigned char buf[8192];
        uint32_t n = 0;
        db_err e = db_blob_read_frame(db, (db_blob_id)id, frame, buf, sizeof(buf), &n);
        if (e != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_strerror(e));
            return 1;
        }
        printf("帧 %llu (%u 字节):\n", frame, n);
        fwrite(buf, 1, n, stdout);
        printf("\n");
        return 1;
    }

    if (strncmp(line, "STAT", 4) == 0) {
        line += 4;
        unsigned long long id = strtoull(line, NULL, 10);

        uint64_t nframes = 0, total = 0;
        uint32_t fsize = 0;
        db_err e = db_blob_stat(db, (db_blob_id)id, &nframes, &fsize, &total);
        if (e != DB_OK) {
            fprintf(stderr, "错误: %s\n", db_strerror(e));
            return 1;
        }
        printf("blob %llu: %llu 帧 × %u 字节 = %llu 字节\n",
               id, (unsigned long long)nframes, fsize,
               (unsigned long long)total);
        return 1;
    }

    if (strncmp(line, "LIST", 4) == 0) {
        uint32_t n = db_blob_count(db);
        printf("%u 个对象:\n", n);
        for (uint32_t i = 0; i < n; i++) {
            db_blob_id id = db_blob_id_at(db, i);
            uint64_t nf = 0, total = 0;
            uint32_t fs = 0;
            if (db_blob_stat(db, id, &nf, &fs, &total) == DB_OK)
                printf("  blob %llu: %llu 帧, %llu 字节\n",
                       (unsigned long long)id, (unsigned long long)nf,
                       (unsigned long long)total);
        }
        return 1;
    }

    fprintf(stderr, "错误: 未知的 BLOB 子命令\n");
    return 1;
}
#endif
