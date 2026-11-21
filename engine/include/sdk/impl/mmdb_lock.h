/**
 * @file mmdb_lock.h
 * @brief SDK 自持的跨平台读写锁原语。
 *
 * 原先只是转发冻结轨锁头的兼容 shim；冻结轨退役后 SDK 不能再依赖它，
 * 因此这里把平台分支和声明复制进 SDK 自身，实现见 `src/sdk/core/mmdb_lock.c`。
 */
#ifndef SDK_IMPL_MMDB_LOCK_H
#define SDK_IMPL_MMDB_LOCK_H

#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK mmdb_rwlock_t;
#else
#include <pthread.h>
typedef pthread_rwlock_t mmdb_rwlock_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

int mmdb_rwlock_init(mmdb_rwlock_t *lock);
int mmdb_rwlock_rdlock(mmdb_rwlock_t *lock);
int mmdb_rwlock_wrlock(mmdb_rwlock_t *lock);
int mmdb_rwlock_unlock(mmdb_rwlock_t *lock, int is_wrlock);
int mmdb_rwlock_destroy(mmdb_rwlock_t *lock);

#ifdef __cplusplus
}
#endif

#endif /* SDK_IMPL_MMDB_LOCK_H */
