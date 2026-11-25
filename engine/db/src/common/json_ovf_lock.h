#ifndef DB_COMMON_JSON_OVF_LOCK_H
#define DB_COMMON_JSON_OVF_LOCK_H

/*
 * json_ovf_lock.h —— JSON 页链的「表级读/释放互斥」（C5.3b-b Task 5）
 *
 * ==================== 为什么需要一把表级锁 ====================
 *
 * 一条 JSON 页链跨多页。读路径要沿 next 走完整条链才算读到完整文档，
 * 而 VACUUM 回收链时是逐页清零 + 还页。若两者并发：
 *
 *   读者刚读完第 1 页 → VACUUM 把第 2..N 页清零并还给分配器
 *   → 读者读第 2 页时页头已不是 PT_JSON_OVF ⇒ E_CORRUPT
 *
 * 报错本身不丢数据，但"库自己回收空间就能让查询报损坏"是不可接受的
 * 用户体验，而且会让 E_CORRUPT 失去"真的坏了"的诊断意义。
 *
 * 【为什么不在链页上加 frame 锁】
 *
 * 读一条链要在多个页上各取一次内容锁。而现有的锁全序是
 * "frame → btree / frame → wal"，跨页同时持有多把 frame 锁会引入
 * 新的锁序组合 ⇒ 死锁配方。表级读写锁只有一把，不产生新组合。
 *
 * ==================== 为什么放在 L0（common/） ====================
 *
 * 这把锁的**使用者**分布在多个层：
 *   - L1 storage/heap.c         ：每张表持有它（Heap 结构的一个字段）
 *   - L1 storage/heap.h         ：暴露访问器给上层
 *   - L4 modalities/doc         ：读链时取读锁
 *   - L4 modalities/relational  ：读/回收链时取读写锁
 *
 * 放在任何一层都会让别的层反向依赖。放在 L0，谁都能用，
 * 而且它只是 DbRwLock 的一层薄封装，不带任何 JSON 知识。
 */

#include "common/rwlock.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JsonOvfTableLock {
    DbRwLock rw;
} JsonOvfTableLock;

void json_ovf_table_lock_init(JsonOvfTableLock *lock);
void json_ovf_table_lock_destroy(JsonOvfTableLock *lock);

int json_ovf_table_read_lock(JsonOvfTableLock *lock);
int json_ovf_table_read_unlock(JsonOvfTableLock *lock);
int json_ovf_table_write_lock(JsonOvfTableLock *lock);
int json_ovf_table_write_unlock(JsonOvfTableLock *lock);

/*
 * 给"可以没有锁"的调用方用的空安全包装：
 * lock == NULL 视为"无需加锁"，直接返回 OK。
 *
 * 【为什么单独一对而不是让上面四个函数接受 NULL】
 * 上面四个是**原语**：传 NULL 是编程错误，该报 E_INVAL。
 * 而读写路径上 lock == NULL 是**正常情形**（写路径的值还没归属），
 * 两者语义不同，混在一起会让"忘了传锁"也静默通过。
 */
int json_ovf_lock_rdlock(JsonOvfTableLock *lock);
int json_ovf_lock_rdunlock(JsonOvfTableLock *lock);
int json_ovf_lock_wrlock(JsonOvfTableLock *lock);
int json_ovf_lock_wrunlock(JsonOvfTableLock *lock);

#ifdef __cplusplus
}
#endif

#endif /* DB_COMMON_JSON_OVF_LOCK_H */
