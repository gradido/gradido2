/*
 * The table half of the session cache. Architecture.md, *Session cache*, is the specification;
 * this file is only its transcription, and where the two disagree the document is right.
 *
 * Not yet covered by a test or by TSan. AGENTS.md section 4 makes both a condition of putting
 * this on a request path, and section 5 makes reading the design document a condition of
 * changing it. Neither is satisfied by the fact that it compiles.
 */
#include "service_core/cache.h"

#include <stdlib.h>
#include <string.h>

#include "service_core/log.h"
#include "service_core/thread.h"

struct sc_cache_entry {
    /* Table pointer plus every request currently using it. Freed at zero, never anywhere
     * else -- reaching zero implies unreachable, because the table drops its pointer before it
     * gives up its reference. */
    volatile int32_t references;
    uint32_t key_len;
    char key[SC_CACHE_KEY_MAX];
    int64_t created_ms;
    void *value;
    sc_cache *owner;
};

struct sc_cache {
    sc_rwlock *lock;
    sc_cache_entry **slots;
    uint32_t mask; /* capacity - 1, capacity being a power of two */
    uint32_t probe;
    int64_t hard_timeout_ms;
    sc_cache_free_fn free_value;
    void *user_data;
};

static uint32_t round_up_pow2(uint32_t value)
{
    uint32_t result = 1;
    while (result < value && result < 0x40000000u)
        result <<= 1;
    return result;
}

/* FNV-1a. The key is a digest already, so the hash only has to spread it over the slots. */
static uint64_t hash_key(const char *key, size_t key_len)
{
    uint64_t hash = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < key_len; ++i) {
        hash ^= (unsigned char)key[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static int entry_expired(const sc_cache *cache, const sc_cache_entry *entry, int64_t now_ms)
{
    if (cache->hard_timeout_ms <= 0)
        return 0;
    return now_ms - entry->created_ms >= cache->hard_timeout_ms;
}

static int entry_matches(const sc_cache_entry *entry, const char *key, size_t key_len)
{
    return entry->key_len == (uint32_t)key_len && memcmp(entry->key, key, key_len) == 0;
}

sc_cache *sc_cache_create(const sc_cache_config *cfg)
{
    sc_cache *cache;
    uint32_t capacity;

    if (cfg == NULL || cfg->capacity == 0)
        return NULL;
    cache = (sc_cache *)calloc(1, sizeof(*cache));
    if (cache == NULL)
        return NULL;

    capacity = round_up_pow2(cfg->capacity);
    cache->slots = (sc_cache_entry **)calloc(capacity, sizeof(*cache->slots));
    cache->lock = sc_rwlock_create();
    if (cache->slots == NULL || cache->lock == NULL) {
        sc_rwlock_destroy(cache->lock);
        free(cache->slots);
        free(cache);
        return NULL;
    }
    cache->mask = capacity - 1;
    cache->probe = cfg->probe != 0 ? cfg->probe : SC_CACHE_PROBE_DEFAULT;
    if (cache->probe > capacity)
        cache->probe = capacity;
    cache->hard_timeout_ms = cfg->hard_timeout_ms;
    cache->free_value = cfg->free_value;
    cache->user_data = cfg->user_data;
    return cache;
}

void sc_cache_destroy(sc_cache *cache)
{
    uint32_t i;

    if (cache == NULL)
        return;
    /* Give up the table's reference on everything still in it. An entry a request is holding
     * survives this and is freed by that request -- which means the cache must outlive its
     * readers, and that is a startup/shutdown ordering rule, not something to check here. */
    for (i = 0; i <= cache->mask; ++i) {
        sc_cache_entry *entry = cache->slots[i];
        cache->slots[i] = NULL;
        sc_cache_release(entry);
    }
    sc_rwlock_destroy(cache->lock);
    free(cache->slots);
    free(cache);
}

sc_cache_entry *sc_cache_get(sc_cache *cache, const char *key, size_t key_len)
{
    uint64_t hash;
    uint32_t i;
    int64_t now_ms;
    sc_cache_entry *found = NULL;

    if (cache == NULL || key == NULL || key_len == 0 || key_len > SC_CACHE_KEY_MAX)
        return NULL;
    hash = hash_key(key, key_len);
    now_ms = sc_now_ms();

    sc_rwlock_rdlock(cache->lock);
    for (i = 0; i < cache->probe; ++i) {
        sc_cache_entry *entry = cache->slots[(uint32_t)(hash + i) & cache->mask];
        if (entry == NULL)
            continue;
        if (!entry_matches(entry, key, key_len))
            continue;
        /* An expired entry reads as absent. A reader holds only the shared lock and therefore
         * does not reclaim it; the next put that walks this slot does, and that put is
         * imminent because the miss this reader just took is what triggers it. */
        if (entry_expired(cache, entry, now_ms))
            break;
        /* Inside the lock. After it would be a use-after-free: in the gap an evictor can drop
         * the pointer, the count can fall to zero, and the increment lands in freed memory. */
        (void)sc_atomic_inc(&entry->references);
        found = entry;
        break;
    }
    sc_rwlock_rdunlock(cache->lock);
    return found;
}

sc_cache_entry *sc_cache_put(sc_cache *cache, const char *key, size_t key_len, void *value)
{
    sc_cache_entry *entry;
    sc_cache_entry *displaced = NULL;
    uint64_t hash;
    uint32_t i;
    uint32_t victim;
    int victim_rank = 4; /* 0 same key, 1 free, 2 expired, 3 unused, 4 first candidate */
    int64_t now_ms;

    if (cache == NULL || key == NULL || key_len == 0 || key_len > SC_CACHE_KEY_MAX)
        return NULL;

    /* The miss path may allocate; the lookup path may not. */
    entry = (sc_cache_entry *)calloc(1, sizeof(*entry));
    if (entry == NULL)
        return NULL;
    entry->references = 2; /* one for the table, one for the caller */
    entry->key_len = (uint32_t)key_len;
    memcpy(entry->key, key, key_len);
    entry->created_ms = sc_now_ms();
    entry->value = value;
    entry->owner = cache;

    hash = hash_key(key, key_len);
    now_ms = entry->created_ms;
    victim = (uint32_t)hash & cache->mask;

    sc_rwlock_wrlock(cache->lock);
    /* Insertion never fails. The same key comes first and ends the walk -- taking a free slot
     * while an entry with this key sits further along the window would leave two, and a later
     * lookup would find whichever the probe order reached first. After that: a free slot, an
     * expired entry, one nobody is using, otherwise the first candidate. Preferring an idle
     * victim keeps sessions that are actively serving requests inside the table. */
    for (i = 0; i < cache->probe; ++i) {
        uint32_t slot = (uint32_t)(hash + i) & cache->mask;
        sc_cache_entry *occupant = cache->slots[slot];
        int rank;

        if (occupant != NULL && entry_matches(occupant, key, key_len))
            rank = 0;
        else if (occupant == NULL)
            rank = 1;
        else if (entry_expired(cache, occupant, now_ms))
            rank = 2;
        else if (sc_atomic_load(&occupant->references) <= 1)
            rank = 3;
        else
            rank = 4;

        if (rank < victim_rank) {
            victim_rank = rank;
            victim = slot;
        }
        if (rank == 0)
            break;
    }
    displaced = cache->slots[victim];
    cache->slots[victim] = entry;
    sc_rwlock_wrunlock(cache->lock);

    /* Outside the lock: the last release runs the value's free callback, which is caller code
     * and may do anything, including take a lock of its own. */
    sc_cache_release(displaced);
    return entry;
}

void *sc_cache_entry_value(const sc_cache_entry *entry)
{
    return entry != NULL ? entry->value : NULL;
}

void sc_cache_release(sc_cache_entry *entry)
{
    if (entry == NULL)
        return;
    /* No lock. The table no longer points at an entry whose count can reach zero, so nothing
     * can revive it; the acq_rel on the decrement is what makes the last user's writes visible
     * before the memory is reused. */
    if (sc_atomic_dec(&entry->references) != 0)
        return;
    if (entry->owner != NULL && entry->owner->free_value != NULL)
        entry->owner->free_value(entry->value, entry->owner->user_data);
    free(entry);
}
