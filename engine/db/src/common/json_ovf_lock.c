/*
 * json_ovf_lock.c —— 表级 JSON 页链锁的实现（C5.3b-b Task 5）
 *
 * 实现说明见 common/json_ovf_lock.h：这里只是 DbRwLock 的薄转发。
 *
 * 【为什么原语（四个 table_*）对 NULL 报 E_INVAL，而包装（四个 lock_*）放过】
 *
 * 原语的调用点都是"我已经确定有锁，只是要用它" —— 传 NULL 说明有 bug，
 * 越早报越好。包装的调用点（读链 / 回收链）在写路径上本来就没有表锁，
 * NULL 是正常参数 —— 那里报错会让合法的写路径失败。
 * 两套语义分开，就不会出现"忘了传锁却静默成功"。
 */

#include "common/json_ovf_lock.h"

#include "common/error.h"

void json_ovf_table_lock_init(JsonOvfTableLock *lock)
{
    if (lock)
        db_rwlock_init(&lock->rw);
}

void json_ovf_table_lock_destroy(JsonOvfTableLock *lock)
{
    if (lock)
        db_rwlock_destroy(&lock->rw);
}

int json_ovf_table_read_lock(JsonOvfTableLock *lock)
{
    if (!lock)
        return E_INVAL;
    db_rwlock_rdlock(&lock->rw);
    return OK;
}

int json_ovf_table_read_unlock(JsonOvfTableLock *lock)
{
    if (!lock)
        return E_INVAL;
    db_rwlock_rdunlock(&lock->rw);
    return OK;
}

int json_ovf_table_write_lock(JsonOvfTableLock *lock)
{
    if (!lock)
        return E_INVAL;
    db_rwlock_wrlock(&lock->rw);
    return OK;
}

int json_ovf_table_write_unlock(JsonOvfTableLock *lock)
{
    if (!lock)
        return E_INVAL;
    db_rwlock_wrunlock(&lock->rw);
    return OK;
}

int json_ovf_lock_rdlock(JsonOvfTableLock *lock)
{
    return lock ? json_ovf_table_read_lock(lock) : OK;
}

int json_ovf_lock_rdunlock(JsonOvfTableLock *lock)
{
    return lock ? json_ovf_table_read_unlock(lock) : OK;
}

int json_ovf_lock_wrlock(JsonOvfTableLock *lock)
{
    return lock ? json_ovf_table_write_lock(lock) : OK;
}

int json_ovf_lock_wrunlock(JsonOvfTableLock *lock)
{
    return lock ? json_ovf_table_write_unlock(lock) : OK;
}
