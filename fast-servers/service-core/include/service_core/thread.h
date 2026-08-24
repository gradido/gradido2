/*
 * The threading primitives the fast servers need, and no more.
 *
 * pthreads on posix, the Win32 API on Windows. It exists because the process runs one role per
 * thread and because the session cache needs a shared/exclusive lock and an atomic counter --
 * neither of which C11 offers portably: <threads.h> has no reader/writer lock at all, and MSVC
 * ships C11 atomics only behind an experimental switch.
 *
 * Everything here is created at startup. Nothing in this header allocates on the request path.
 */
#ifndef SERVICE_CORE_THREAD_H
#define SERVICE_CORE_THREAD_H

#include <stdint.h>

#include "service_core/status.h"

typedef struct sc_thread sc_thread;
typedef struct sc_mutex sc_mutex;
typedef struct sc_rwlock sc_rwlock;

/* The thread body. Its return value reaches the caller through sc_thread_join. */
typedef int (*sc_thread_fn)(void *arg);

sc_status sc_thread_start(sc_thread **out, sc_thread_fn fn, void *arg);
/** Waits for the thread and frees the handle. @p exit_code may be NULL. */
sc_status sc_thread_join(sc_thread *thread, int *exit_code);

sc_mutex *sc_mutex_create(void);
void sc_mutex_destroy(sc_mutex *mutex);
void sc_mutex_lock(sc_mutex *mutex);
void sc_mutex_unlock(sc_mutex *mutex);

/*
 * Shared/exclusive. The session cache's table lock is one of these, and the order in which it
 * is taken is not a matter of taste -- see Architecture.md, *Session cache*.
 */
sc_rwlock *sc_rwlock_create(void);
void sc_rwlock_destroy(sc_rwlock *lock);
void sc_rwlock_rdlock(sc_rwlock *lock);
void sc_rwlock_wrlock(sc_rwlock *lock);
/* One unlock for both modes: SRWLOCK needs to know which it is releasing, so the caller says. */
void sc_rwlock_rdunlock(sc_rwlock *lock);
void sc_rwlock_wrunlock(sc_rwlock *lock);

/*
 * Reference counting. Both return the value after the operation.
 *
 * The increment may be relaxed -- it happens inside the table's shared lock, which is what
 * orders it. The decrement is acquire/release, so the last user's writes are visible before
 * the memory is handed back. Architecture.md, *Reference counting*, holds why.
 */
int32_t sc_atomic_inc(volatile int32_t *value);
int32_t sc_atomic_dec(volatile int32_t *value);
int32_t sc_atomic_load(const volatile int32_t *value);
/**
 * Release store. Async-signal-safe on every target this builds for, which is what lets the
 * signal handler in runtime.c use it -- C11 7.14.1.1 allows a handler to touch a lock-free
 * atomic object, and nothing else this project has.
 */
void sc_atomic_store(volatile int32_t *value, int32_t desired);

#endif /* SERVICE_CORE_THREAD_H */
