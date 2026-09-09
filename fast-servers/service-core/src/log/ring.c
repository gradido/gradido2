#include "ring.h"

#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "arnm/dynamic_arena_pool.h"
#include "state.h"

static sc_log_slot *g_ring;
static uint64_t g_capacity;
static uint64_t g_mask;
static _Atomic uint64_t g_head; /* next ticket; fetch_add only */
static uint64_t g_tail;         /* belongs to the logger alone */

/*
 * The three that need making. libuv has no static initializer to offer -- a Windows critical
 * section is not a value the way PTHREAD_MUTEX_INITIALIZER is -- so they are made in
 * sc_log_ring_init() and unmade in sc_log_ring_free(). g_sync_ready says which of the two
 * states this is: sc_log_shutdown() may run after a sc_log_init() that failed halfway.
 */
static uv_mutex_t g_mtx;
static uv_cond_t g_cv_notfull;
static uv_cond_t g_cv_notempty;
static int g_sync_ready;

static _Atomic uint32_t g_producer_waiters;
static _Atomic int g_consumer_waiting;
static _Atomic int g_running;

static sc_log_owner g_owners[SC_LOG_MAX_THREADS];
/* _Thread_local and not __thread: the same storage, spelled the way C11 spells it, so that the
 * MSVC build has it too. */
static _Thread_local int t_owner = -1;

/*
 * How long the logger spins on an empty ring before going to sleep. Waking a sleeping
 * consumer costs the submitter a mutex and a futex call -- at any log rate worth the name
 * spinning is cheaper than waking, and at a quiet one the spin costs one core a few
 * microseconds and then nothing at all.
 */
#define SC_LOG_IDLE_SPINS 20000u

static void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    (void)0;
#endif
}

static uint64_t round_pow2(uint64_t v)
{
    uint64_t p = 8;
    while (p < v)
        p <<= 1;
    return p;
}

/* ------------------------------------------------------------------ The ring */

int sc_log_ring_init(uint32_t requested)
{
    uint64_t i;

    if (!g_sync_ready) {
        if (uv_mutex_init(&g_mtx) != 0)
            return -1;
        if (uv_cond_init(&g_cv_notfull) != 0) {
            uv_mutex_destroy(&g_mtx);
            return -1;
        }
        if (uv_cond_init(&g_cv_notempty) != 0) {
            uv_cond_destroy(&g_cv_notfull);
            uv_mutex_destroy(&g_mtx);
            return -1;
        }
        g_sync_ready = 1;
    }

    g_capacity = round_pow2(requested ? requested : 4096);
    g_mask = g_capacity - 1;
    g_ring = (sc_log_slot *)calloc((size_t)g_capacity, sizeof(*g_ring));
    if (g_ring == NULL)
        return -1;
    for (i = 0; i < g_capacity; ++i)
        atomic_init(&g_ring[i].seq, i);
    atomic_store(&g_head, 0);
    g_tail = 0;
    atomic_store(&g_running, 1);
    return 0;
}

void sc_log_ring_free(void)
{
    free(g_ring);
    g_ring = NULL;
    if (g_sync_ready) {
        uv_cond_destroy(&g_cv_notempty);
        uv_cond_destroy(&g_cv_notfull);
        uv_mutex_destroy(&g_mtx);
        g_sync_ready = 0;
    }
}

void sc_log_ring_stop(void)
{
    atomic_store_explicit(&g_running, 0, memory_order_release);
    uv_mutex_lock(&g_mtx);
    uv_cond_broadcast(&g_cv_notempty);
    uv_cond_broadcast(&g_cv_notfull);
    uv_mutex_unlock(&g_mtx);
}

uint64_t sc_log_ring_capacity(void) { return g_capacity; }

uint64_t sc_log_ring_fill(void)
{
    return atomic_load_explicit(&g_head, memory_order_acquire) - g_tail;
}

/* ------------------------------------------------------------------ Producer */

sc_log_slot *sc_log_ring_reserve(uint64_t *out_pos)
{
    /* The index is monotonic and the slot will become free -- so wait rather than drop. This
     * is the backpressure, and it is deliberate. */
    uint64_t pos = atomic_fetch_add_explicit(&g_head, 1, memory_order_relaxed);
    sc_log_slot *s = &g_ring[pos & g_mask];

    if (atomic_load_explicit(&s->seq, memory_order_acquire) != pos) {
        atomic_fetch_add_explicit(&g_sc_log_n_blocked, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_producer_waiters, 1, memory_order_release);
        uv_mutex_lock(&g_mtx);
        while (atomic_load_explicit(&s->seq, memory_order_acquire) != pos) {
            /* The logger may be asleep because it has not seen us yet. */
            uv_cond_signal(&g_cv_notempty);
            uv_cond_wait(&g_cv_notfull, &g_mtx);
        }
        uv_mutex_unlock(&g_mtx);
        atomic_fetch_sub_explicit(&g_producer_waiters, 1, memory_order_release);
    }
    *out_pos = pos;
    return s;
}

void sc_log_ring_publish(uint64_t pos)
{
    sc_log_slot *s = &g_ring[pos & g_mask];

    atomic_store_explicit(&s->seq, pos + 1, memory_order_release);
    atomic_fetch_add_explicit(&g_sc_log_n_submitted, 1, memory_order_relaxed);

    if (atomic_load_explicit(&g_consumer_waiting, memory_order_acquire)) {
        uv_mutex_lock(&g_mtx);
        uv_cond_signal(&g_cv_notempty);
        uv_mutex_unlock(&g_mtx);
    }
}

/* ------------------------------------------------------------------ Consumer */

int sc_log_ring_take(sc_log_slot **out, void (*before_sleep)(void))
{
    for (;;) {
        uint64_t head = atomic_load_explicit(&g_head, memory_order_acquire);
        sc_log_slot *s;
        unsigned spins;

        if (g_tail == head) {
            unsigned idle;
            if (!atomic_load_explicit(&g_running, memory_order_acquire))
                return 0;
            for (idle = 0; idle < SC_LOG_IDLE_SPINS; ++idle) {
                if (g_tail != atomic_load_explicit(&g_head, memory_order_acquire))
                    break;
                cpu_relax();
            }
            if (g_tail != atomic_load_explicit(&g_head, memory_order_acquire))
                continue;
            if (before_sleep != NULL)
                before_sleep();
            uv_mutex_lock(&g_mtx);
            atomic_store_explicit(&g_consumer_waiting, 1, memory_order_release);
            while (g_tail == atomic_load_explicit(&g_head, memory_order_acquire) &&
                   atomic_load_explicit(&g_running, memory_order_acquire))
                uv_cond_wait(&g_cv_notempty, &g_mtx);
            atomic_store_explicit(&g_consumer_waiting, 0, memory_order_release);
            uv_mutex_unlock(&g_mtx);
            continue;
        }

        s = &g_ring[g_tail & g_mask];
        /* Wait for the commit: the ticket is taken, the producer is still writing. */
        spins = 0;
        while (atomic_load_explicit(&s->seq, memory_order_acquire) != g_tail + 1) {
            if (++spins > 128u) {
                /* libuv has no uv_thread_yield; a sleep of zero is the same ask -- hand the
                 * rest of this slice back -- on both platforms it covers. */
                uv_sleep(0);
                spins = 0;
            }
        }
        *out = s;
        return 1;
    }
}

void sc_log_ring_done(sc_log_slot *slot)
{
    /* Release the slot: seq = pos + capacity is the ticket that reaches it next. */
    atomic_store_explicit(&slot->seq, g_tail + g_capacity, memory_order_release);
    g_tail += 1;

    if (atomic_load_explicit(&g_producer_waiters, memory_order_acquire) != 0) {
        uv_mutex_lock(&g_mtx);
        uv_cond_broadcast(&g_cv_notfull);
        uv_mutex_unlock(&g_mtx);
    }
}

/* ------------------------------------------------------------------ Owners */

int sc_log_owner_index(void) { return t_owner; }

sc_log_owner *sc_log_owner_at(int index) { return &g_owners[index]; }

void sc_log_owner_return(sc_log_owner *o, arnm *arena)
{
    uint64_t h = atomic_load_explicit(&o->ret_head, memory_order_relaxed);
    o->ret[h & o->ret_mask] = arena;
    atomic_store_explicit(&o->ret_head, h + 1, memory_order_release);
}

/* The owner drains its own return queue. Only it calls pool_free -- the logger never does. */
void sc_log_owner_reclaim(sc_log_owner *o)
{
    uint64_t t = atomic_load_explicit(&o->ret_tail, memory_order_relaxed);
    uint64_t h = atomic_load_explicit(&o->ret_head, memory_order_acquire);
    while (t != h) {
        arnm *a = o->ret[t & o->ret_mask];
        (void)arnm_graded_arena_pool_free(&o->pool, a);
        ++t;
    }
    atomic_store_explicit(&o->ret_tail, t, memory_order_release);
}

uint64_t sc_log_owner_host_allocs_total(void)
{
    uint64_t total = 0;
    int i;
    for (i = 0; i < SC_LOG_MAX_THREADS; ++i)
        total += g_owners[i].host_allocs;
    return total;
}

int sc_log_owner_join(void)
{
    int i;
    uint64_t ret_cap;

    if (t_owner >= 0)
        return t_owner;
    for (i = 0; i < SC_LOG_MAX_THREADS; ++i) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_owners[i].in_use, &expected, 1))
            break;
    }
    if (i == SC_LOG_MAX_THREADS)
        return -1;

    /* Large enough that a return can never queue up: no more arenas can be in flight at
     * once than the ring has slots. */
    ret_cap = g_capacity * 2u;
    g_owners[i].ret = (arnm **)calloc((size_t)ret_cap, sizeof(arnm *));
    if (g_owners[i].ret == NULL) {
        atomic_store(&g_owners[i].in_use, 0);
        return -1;
    }
    g_owners[i].ret_mask = (uint32_t)(ret_cap - 1);
    atomic_store(&g_owners[i].ret_head, 0);
    atomic_store(&g_owners[i].ret_tail, 0);
    g_owners[i].host_allocs = 0;

    /*
     * The free list's ceiling has to be the ring capacity, not the prefill: otherwise arnm
     * hands every arena beyond it back to the host and the next request mallocs it again --
     * precisely under load. The ceiling costs nothing as long as the arenas do not actually
     * come into being; only the prefilled ones are created.
     */
    if (arnm_graded_arena_pool_init(&g_owners[i].pool, kScLogGrades, SC_LOG_GRADE_COUNT,
                                    (uint32_t)g_capacity) != ARNM_SUCCESS) {
        free(g_owners[i].ret);
        atomic_store(&g_owners[i].in_use, 0);
        return -1;
    }
    /* Prefill the stock so steady state never sees the allocator. */
    {
        uint16_t g;
        for (g = 0; g < SC_LOG_GRADE_COUNT; ++g)
            (void)arnm_dynamic_arena_pool_reserve(
                arnm_graded_arena_pool_grade_at(&g_owners[i].pool, g),
                g_sc_log_cfg.spare_per_grade);
    }
    t_owner = i;
    return i;
}

void sc_log_owner_leave(void)
{
    sc_log_owner *o;
    uint16_t g;
    int outstanding = 1;

    if (t_owner < 0)
        return;
    o = &g_owners[t_owner];

    /* Wait until the logger has put back every arena of ours. It is still running. */
    while (outstanding) {
        sc_log_owner_reclaim(o);
        outstanding = 0;
        for (g = 0; g < SC_LOG_GRADE_COUNT; ++g) {
            arnm_dynamic_arena_pool *grade = arnm_graded_arena_pool_grade_at(&o->pool, g);
            if (grade != NULL && grade->acquired_count != 0)
                outstanding = 1;
        }
        if (outstanding)
            uv_sleep(0);
    }
    (void)arnm_graded_arena_pool_release(&o->pool);
    free(o->ret);
    o->ret = NULL;
    atomic_store(&o->in_use, 0);
    t_owner = -1;
}
