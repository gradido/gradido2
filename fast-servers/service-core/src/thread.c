/*
 * pthreads on posix, the Win32 API on Windows. See the header for why this exists at all.
 */
#include "service_core/thread.h"

#include <stdlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

struct sc_thread {
#if defined(_WIN32)
    HANDLE handle;
#else
    pthread_t handle;
#endif
    sc_thread_fn fn;
    void *arg;
    int exit_code;
};

struct sc_mutex {
#if defined(_WIN32)
    CRITICAL_SECTION impl;
#else
    pthread_mutex_t impl;
#endif
};

struct sc_rwlock {
#if defined(_WIN32)
    SRWLOCK impl;
#else
    pthread_rwlock_t impl;
#endif
};

#if defined(_WIN32)
static DWORD WINAPI thread_trampoline(LPVOID arg)
{
    sc_thread *thread = (sc_thread *)arg;
    thread->exit_code = thread->fn(thread->arg);
    return 0;
}
#else
static void *thread_trampoline(void *arg)
{
    sc_thread *thread = (sc_thread *)arg;
    thread->exit_code = thread->fn(thread->arg);
    return NULL;
}
#endif

sc_status sc_thread_start(sc_thread **out, sc_thread_fn fn, void *arg)
{
    sc_thread *thread;

    if (out == NULL || fn == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    thread = (sc_thread *)calloc(1, sizeof(*thread));
    if (thread == NULL)
        return SC_ERR_NO_MEMORY;
    thread->fn = fn;
    thread->arg = arg;

#if defined(_WIN32)
    thread->handle = CreateThread(NULL, 0, thread_trampoline, thread, 0, NULL);
    if (thread->handle == NULL) {
        free(thread);
        return SC_ERR_NO_MEMORY;
    }
#else
    if (pthread_create(&thread->handle, NULL, thread_trampoline, thread) != 0) {
        free(thread);
        return SC_ERR_NO_MEMORY;
    }
#endif
    *out = thread;
    return SC_OK;
}

sc_status sc_thread_join(sc_thread *thread, int *exit_code)
{
    if (thread == NULL)
        return SC_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
#else
    pthread_join(thread->handle, NULL);
#endif
    if (exit_code != NULL)
        *exit_code = thread->exit_code;
    free(thread);
    return SC_OK;
}

sc_mutex *sc_mutex_create(void)
{
    sc_mutex *mutex = (sc_mutex *)calloc(1, sizeof(*mutex));
    if (mutex == NULL)
        return NULL;
#if defined(_WIN32)
    InitializeCriticalSection(&mutex->impl);
#else
    if (pthread_mutex_init(&mutex->impl, NULL) != 0) {
        free(mutex);
        return NULL;
    }
#endif
    return mutex;
}

void sc_mutex_destroy(sc_mutex *mutex)
{
    if (mutex == NULL)
        return;
#if defined(_WIN32)
    DeleteCriticalSection(&mutex->impl);
#else
    (void)pthread_mutex_destroy(&mutex->impl);
#endif
    free(mutex);
}

void sc_mutex_lock(sc_mutex *mutex)
{
#if defined(_WIN32)
    EnterCriticalSection(&mutex->impl);
#else
    (void)pthread_mutex_lock(&mutex->impl);
#endif
}

void sc_mutex_unlock(sc_mutex *mutex)
{
#if defined(_WIN32)
    LeaveCriticalSection(&mutex->impl);
#else
    (void)pthread_mutex_unlock(&mutex->impl);
#endif
}

sc_rwlock *sc_rwlock_create(void)
{
    sc_rwlock *lock = (sc_rwlock *)calloc(1, sizeof(*lock));
    if (lock == NULL)
        return NULL;
#if defined(_WIN32)
    InitializeSRWLock(&lock->impl);
#else
    if (pthread_rwlock_init(&lock->impl, NULL) != 0) {
        free(lock);
        return NULL;
    }
#endif
    return lock;
}

void sc_rwlock_destroy(sc_rwlock *lock)
{
    if (lock == NULL)
        return;
#if !defined(_WIN32)
    /* SRWLOCK has no destructor; it is valid for as long as its storage is. */
    (void)pthread_rwlock_destroy(&lock->impl);
#endif
    free(lock);
}

void sc_rwlock_rdlock(sc_rwlock *lock)
{
#if defined(_WIN32)
    AcquireSRWLockShared(&lock->impl);
#else
    (void)pthread_rwlock_rdlock(&lock->impl);
#endif
}

void sc_rwlock_wrlock(sc_rwlock *lock)
{
#if defined(_WIN32)
    AcquireSRWLockExclusive(&lock->impl);
#else
    (void)pthread_rwlock_wrlock(&lock->impl);
#endif
}

void sc_rwlock_rdunlock(sc_rwlock *lock)
{
#if defined(_WIN32)
    ReleaseSRWLockShared(&lock->impl);
#else
    (void)pthread_rwlock_unlock(&lock->impl);
#endif
}

void sc_rwlock_wrunlock(sc_rwlock *lock)
{
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&lock->impl);
#else
    (void)pthread_rwlock_unlock(&lock->impl);
#endif
}

int32_t sc_atomic_inc(volatile int32_t *value)
{
#if defined(_WIN32) && !defined(__GNUC__)
    return (int32_t)InterlockedIncrement((volatile LONG *)value);
#else
    return __atomic_add_fetch(value, 1, __ATOMIC_RELAXED);
#endif
}

int32_t sc_atomic_dec(volatile int32_t *value)
{
#if defined(_WIN32) && !defined(__GNUC__)
    return (int32_t)InterlockedDecrement((volatile LONG *)value);
#else
    /* acq_rel, so the last user's writes are visible to whoever frees the memory. */
    return __atomic_sub_fetch(value, 1, __ATOMIC_ACQ_REL);
#endif
}

int32_t sc_atomic_load(const volatile int32_t *value)
{
#if defined(_WIN32) && !defined(__GNUC__)
    return (int32_t)InterlockedCompareExchange((volatile LONG *)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

void sc_atomic_store(volatile int32_t *value, int32_t desired)
{
#if defined(_WIN32) && !defined(__GNUC__)
    (void)InterlockedExchange((volatile LONG *)value, (LONG)desired);
#else
    __atomic_store_n(value, desired, __ATOMIC_RELEASE);
#endif
}
