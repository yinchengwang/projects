/*
 * test_catalog_lock.c —— v1.2 步骤 3：catalog 域读写锁的验收
 *
 * 7 项：
 *   1. 读锁**可重入**：嵌套两次读锁 → 写者被挡住；放一层仍挡；全放才进
 *   2. 读读**不互斥**：两个线程同时持读锁都能进
 *   3. 写锁**排他**：持写锁时读者进不来
 *   4. **升级**被拒（持读锁取写锁）
 *   5. **降级**被拒（持写锁取读锁）
 *   6. 写锁**不可重入**
 *   7. 默认行为是 **abort**（真·机器证明，用子进程验）
 *
 * ==================== 为什么第 1 项要这么绕 ====================
 *
 * 读锁可重入的**实现**是"计数 +1，只有第一次才碰底层 rwlock"。
 * 如果实现错了（比如每次都真去取读锁），POSIX 规定那是未定义行为，
 * 而且有写者在等时会死锁 —— 但"死锁"在测试里表现为**超时**，很难定位。
 *
 * 所以第 1 项用**写者当探针**：
 * 写者要拿到写锁，必须等所有读者放手。
 * 于是"读者还剩几层"这件事，可以从**外面**看出来：
 *   放 1 层 → 写者还是进不去（说明还剩 1 层）
 *   放 2 层 → 写者进去了（说明全放完了）
 *
 * ==================== 第 7 项为什么要开子进程 ====================
 *
 * 违规的默认处理是 abort() —— 那会直接杀死测试进程。
 * 想验证"它真的会 abort"，就只能让另一个进程去撞它。
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "catalog/catalog.h"
#include "common/error.h"
#include "db_internal.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/catlock.db"
#define MARK "build/catlock_crash.out"

/* ---------------- 违规处理（测试用） ---------------- */

static int g_violations = 0;
static char g_last_msg[256];

static void on_violation(const char *msg)
{
    g_violations++;
    snprintf(g_last_msg, sizeof(g_last_msg), "%s", msg ? msg : "");
}

/* ---------------- 闸门（带超时的等待） ---------------- */

/*
 * state：0=还没开始  1=正在尝试取锁  2=已经拿到锁
 */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             state;
} Gate;

static void gate_init(Gate *g)
{
    pthread_mutex_init(&g->m, NULL);
    pthread_cond_init(&g->c, NULL);
    g->state = 0;
}

static void gate_signal(Gate *g, int state)
{
    pthread_mutex_lock(&g->m);
    g->state = state;
    pthread_cond_broadcast(&g->c);
    pthread_mutex_unlock(&g->m);
}

/* 等到 state >= want；超时返回当前 state（不会一直挂着） */
static int gate_wait(Gate *g, int want, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&g->m);
    while (g->state < want) {
        if (pthread_cond_timedwait(&g->c, &g->m, &ts) == ETIMEDOUT)
            break;
    }
    int s = g->state;
    pthread_mutex_unlock(&g->m);
    return s;
}

/* ---------------- 1) 读锁可重入 ---------------- */

typedef struct {
    Catalog *cat;
    Gate    *g;
} WriterArg;

static void *writer_thread(void *arg)
{
    WriterArg *a = (WriterArg *)arg;
    gate_signal(a->g, 1);                 /* 我要开始写了 */
    catalog_write_lock(a->cat);           /* 有读者在 → 应该被挡住 */
    gate_signal(a->g, 2);                 /* 拿到写锁了 */
    catalog_write_unlock(a->cat);
    return NULL;
}

static void t1_read_lock_is_reentrant(Catalog *cat)
{
    Gate g;
    gate_init(&g);

    catalog_read_lock(cat);
    catalog_read_lock(cat);               /* 第二层：可重入 */

    WriterArg a = { cat, &g };
    pthread_t tw;
    CHECK(pthread_create(&tw, NULL, writer_thread, &a) == 0);

    /* 写者已经开工，但拿不到写锁 */
    CHECK(gate_wait(&g, 1, 1000) == 1);
    CHECK(gate_wait(&g, 2, 200) < 2);      /* 被挡住：还有两层读锁 */

    catalog_read_unlock(cat);             /* 只放一层 */
    CHECK(gate_wait(&g, 2, 200) < 2);      /* 还剩一层 → 仍然挡住 */

    catalog_read_unlock(cat);             /* 全放 */
    CHECK(gate_wait(&g, 2, 2000) == 2);    /* 写者这才能进 */

    pthread_join(tw, NULL);
}

/* ---------------- 2) 读读不互斥 ---------------- */

typedef struct {
    Catalog *cat;
    Gate    *g;
} ReaderArg;

static void *reader_thread(void *arg)
{
    ReaderArg *a = (ReaderArg *)arg;
    catalog_read_lock(a->cat);            /* 主线程正持着读锁 */
    gate_signal(a->g, 2);
    catalog_read_unlock(a->cat);
    return NULL;
}

static void t2_readers_do_not_exclude(Catalog *cat)
{
    Gate g;
    gate_init(&g);

    catalog_read_lock(cat);               /* 主线程先拿读锁 */

    ReaderArg a = { cat, &g };
    pthread_t tr;
    CHECK(pthread_create(&tr, NULL, reader_thread, &a) == 0);

    /* 第二个读者必须能同时拿到读锁 */
    CHECK(gate_wait(&g, 2, 2000) == 2);

    pthread_join(tr, NULL);
    catalog_read_unlock(cat);
}

/* ---------------- 3) 写锁排他 ---------------- */

static void t3_writer_excludes_readers(Catalog *cat)
{
    Gate g;
    gate_init(&g);

    catalog_write_lock(cat);              /* 主线程持写锁 */

    ReaderArg a = { cat, &g };
    pthread_t tr;
    CHECK(pthread_create(&tr, NULL, reader_thread, &a) == 0);

    CHECK(gate_wait(&g, 2, 300) < 2);      /* 读者进不来 */

    catalog_write_unlock(cat);            /* 放写锁 */
    CHECK(gate_wait(&g, 2, 2000) == 2);    /* 读者这才能进 */

    pthread_join(tr, NULL);
}

/* ---------------- 4/5/6) 三条纪律 ---------------- */

static void t4_no_upgrade(Catalog *cat)
{
    g_violations = 0;
    catalog_read_lock(cat);
    catalog_write_lock(cat);              /* 升级 → 应被拒 */
    CHECK(g_violations == 1);
    CHECK(strstr(g_last_msg, "升级") != NULL);
    catalog_read_unlock(cat);
    g_violations = 0;
}

static void t5_no_downgrade(Catalog *cat)
{
    g_violations = 0;
    catalog_write_lock(cat);
    catalog_read_lock(cat);               /* 降级 → 应被拒 */
    CHECK(g_violations == 1);
    CHECK(strstr(g_last_msg, "降级") != NULL);
    catalog_write_unlock(cat);
    g_violations = 0;
}

static void t6_write_lock_not_reentrant(Catalog *cat)
{
    g_violations = 0;
    catalog_write_lock(cat);
    catalog_write_lock(cat);              /* 重复取写锁 → 应被拒 */
    CHECK(g_violations == 1);
    CHECK(strstr(g_last_msg, "重入") != NULL);
    catalog_write_unlock(cat);
    g_violations = 0;
}

/* ---------------- 7) 默认行为真 abort ---------------- */

/*
 * 子进程：撞一次"升级"，让默认处理 abort 掉自己。
 *
 * 先写一个标记文件 —— 万一 system() 没找到二进制，
 * 父进程看到的只是"退出码非 0"，会误判成"abort 成功"。
 * 有了标记文件，才能证明"它真的跑到了那一步"。
 */
static int child_crash_case(void)
{
    FILE *f = fopen(MARK, "w");
    if (f) {
        fprintf(f, "child reached checkpoint\n");
        fclose(f);
    }

    DB *db = NULL;
    /* 用**另一个文件**：父进程正开着 PATH，两个进程同时开一个库会打架 */
    if (db_open("test/tmp/catlock_child.db", &db) != DB_OK)
        return 3;
    Catalog *cat = db->db.cat;

    /* 不装处理函数 → 走默认的"打印 + abort" */
    catalog_set_violation_handler(NULL);

    catalog_read_lock(cat);
    catalog_write_lock(cat);              /* 升级 → abort */
    catalog_read_unlock(cat);

    db_close(db);
    return 0;                              /* 不该走到这里 */
}

static void t7_default_is_abort(void)
{
    remove(MARK);

    /* system() 的 stderr 会污染测试输出，重定向掉 */
#if defined(_WIN32)
    int rc = system("set DB_CATLOCK_CRASH=1 && build\\test_catalog_lock.exe >NUL 2>&1");
#else
    int rc = system("DB_CATLOCK_CRASH=1 ./build/test_catalog_lock.exe >/dev/null 2>&1");
#endif

    /* 子进程被 abort 掉 → 退出码非 0 */
    CHECK(rc != 0);

    /* 而且它真的跑到了那一步（不是"二进制没找到"） */
    FILE *f = fopen(MARK, "r");
    CHECK(f != NULL);
    if (f)
        fclose(f);
    remove(MARK);
}

/* ---------------- main ---------------- */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);      /* 崩溃时也能看到走到哪一步 */

    /* 子进程模式：撞违规，验证默认行为是 abort */
    if (getenv("DB_CATLOCK_CRASH"))
        return child_crash_case();

    printf("test_catalog_lock（单元）:\n");

    remove(PATH);
    remove("test/tmp/catlock.db-wal");
    remove("test/tmp/catlock.db-shm");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return 1;
    Catalog *cat = db->db.cat;
    CHECK(cat != NULL);
    if (!cat)
        return 1;

    /* 装上处理函数：纪律项就不 abort 了，改成"记一笔" */
    catalog_set_violation_handler(on_violation);

    printf("  1. 读锁可重入（写者当探针）\n");
    t1_read_lock_is_reentrant(cat);

    printf("  2. 读读不互斥\n");
    t2_readers_do_not_exclude(cat);

    printf("  3. 写锁排他\n");
    t3_writer_excludes_readers(cat);

    printf("  4. 升级被拒\n");
    t4_no_upgrade(cat);

    printf("  5. 降级被拒\n");
    t5_no_downgrade(cat);

    printf("  6. 写锁不可重入\n");
    t6_write_lock_not_reentrant(cat);

    /* 恢复默认（第 7 项要验的就是默认行为） */
    catalog_set_violation_handler(NULL);

    printf("  7. 默认行为真 abort（子进程）\n");
    t7_default_is_abort();

    db_close(db);

    if (failures == 0) {
        printf("  7 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
