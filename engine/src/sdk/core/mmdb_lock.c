/* SDK read-write lock implementation is sourced by the embedding build. */
#include "sdk/impl/mmdb_lock.h"

#ifdef _WIN32
int mmdb_rwlock_init(mmdb_rwlock_t *lock) { InitializeSRWLock(lock); return 0; }
int mmdb_rwlock_rdlock(mmdb_rwlock_t *lock) { AcquireSRWLockShared(lock); return 0; }
int mmdb_rwlock_wrlock(mmdb_rwlock_t *lock) { AcquireSRWLockExclusive(lock); return 0; }
int mmdb_rwlock_unlock(mmdb_rwlock_t *lock, int is_wrlock)
{
    if (is_wrlock) ReleaseSRWLockExclusive(lock); else ReleaseSRWLockShared(lock);
    return 0;
}
int mmdb_rwlock_destroy(mmdb_rwlock_t *lock) { (void)lock; return 0; }
#else
int mmdb_rwlock_init(mmdb_rwlock_t *lock) { return pthread_rwlock_init(lock, NULL); }
int mmdb_rwlock_rdlock(mmdb_rwlock_t *lock) { return pthread_rwlock_rdlock(lock); }
int mmdb_rwlock_wrlock(mmdb_rwlock_t *lock) { return pthread_rwlock_wrlock(lock); }
int mmdb_rwlock_unlock(mmdb_rwlock_t *lock, int is_wrlock)
{
    (void)is_wrlock;
    return pthread_rwlock_unlock(lock);
}
int mmdb_rwlock_destroy(mmdb_rwlock_t *lock) { return pthread_rwlock_destroy(lock); }
#endif
