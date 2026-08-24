#include "service_core/atomic.h"

#if defined(_WIN32) && !defined(__GNUC__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
