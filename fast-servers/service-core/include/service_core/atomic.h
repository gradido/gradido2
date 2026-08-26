/*
 * The three atomics this project needs, and nothing else.
 *
 * This is what is left of `service_core/thread.h` after the platform layer became libuv:
 * threads, mutexes and reader/writer locks are `uv_thread_*`, `uv_mutex_*` and `uv_rwlock_*`
 * now, used directly. See AGENTS.md section 3a -- one platform seam, maintained once.
 *
 * libuv has no atomics, so these stay ours. They are also the one place where a compiler
 * builtin is the whole implementation: `<stdatomic.h>` would do it in standard C, but MSVC
 * ships C11 atomics only behind an experimental switch and the CMake build has to compile
 * there. Two functions are cheaper than that argument.
 */
#ifndef SERVICE_CORE_ATOMIC_H
#define SERVICE_CORE_ATOMIC_H

#include <stdint.h>

/*
 * Reference counting, and the quit flag. Both return the value after the operation.
 *
 * The increment may be relaxed -- in the session cache it happens inside the table's shared
 * lock, which is what orders it. The decrement is acquire/release, so the last user's writes
 * are visible before the memory is handed back. Architecture.md, *Reference counting*, holds
 * why that asymmetry is the design and not an oversight.
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

/**
 * Compare and swap. Returns non-zero when @p expected was found and @p desired was written.
 *
 * The third atomic, and it arrived with one caller: a deferred request's slot packs its
 * generation and its phase into one word so that "is this ticket still the one this slot
 * holds" and "claim it" are a single operation -- see http_defer.h. Checking the generation
 * and then claiming the slot in two steps is a race, because a slot released and re-armed in
 * between passes both checks while belonging to somebody else.
 */
int sc_atomic_cas(volatile int32_t *value, int32_t expected, int32_t desired);

#endif /* SERVICE_CORE_ATOMIC_H */
