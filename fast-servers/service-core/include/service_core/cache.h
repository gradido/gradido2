/*
 * The table half of the session cache: open addressing, a shared/exclusive lock, reference
 * counted entries, lazy expiry.
 *
 * READ Architecture.md, *Session cache*, IN FULL BEFORE CHANGING ANYTHING HERE. The rules below
 * are not style preferences; AGENTS.md section 5 lists them because each was a bug first.
 *
 *   the table pointer is a reference; the entry is freed exactly when the count reaches zero
 *   the count is incremented INSIDE the table lock, never after releasing it
 *   the table lock is released before any lock inside a value is taken
 *   no lock upgrade -- release shared, take exclusive, check again
 *   no table lock is held across a database call or across the value's free callback
 *
 * What this is not: it holds `void *` values and knows nothing about sessions. The session
 * itself -- its per-data-set locks, its working set -- belongs to backend-core, and the part of
 * that design which is still open (how a working set grows while only a shared lock is held)
 * is recorded in Architecture.md, *Open*. Nothing here decides it.
 *
 * The lookup path does not allocate. Insertion does, and may: it is the miss path.
 */
#ifndef SERVICE_CORE_CACHE_H
#define SERVICE_CORE_CACHE_H

#include <stddef.h>
#include <stdint.h>

typedef struct sc_cache sc_cache;
typedef struct sc_cache_entry sc_cache_entry;

/** Called with the value when its last reference goes away. Registration-time function
 *  pointer, never chosen per call. */
typedef void (*sc_cache_free_fn)(void *value, void *user_data);

/*
 * Keys are fixed size so an entry is one allocation and a lookup is a memcmp.
 *
 * 64 bytes is a digest, not a token: callers key sessions by a hash of the bearer token rather
 * than by the token, so a memory dump of the cache does not hand out credentials.
 */
#define SC_CACHE_KEY_MAX 64

/* Architecture.md: CACHE_PROBE, how far a lookup walks from the slot the hash picked. */
#define SC_CACHE_PROBE_DEFAULT 8

typedef struct sc_cache_config {
    /* Rounded up to a power of two; the probe walk depends on the mask. */
    uint32_t capacity;
    /* 0 selects SC_CACHE_PROBE_DEFAULT. */
    uint32_t probe;
    /* SESSION_HARD_TIMEOUT_MS. 0 disables expiry. */
    int64_t hard_timeout_ms;
    /* Optional; NULL leaves the value to the caller. */
    sc_cache_free_fn free_value;
    void *user_data;
} sc_cache_config;

sc_cache *sc_cache_create(const sc_cache_config *cfg);
/** Drops the table's reference on every entry. An entry a request still holds survives until
 *  that request releases it, which means the cache must outlive its readers. */
void sc_cache_destroy(sc_cache *cache);

/**
 * Looks @p key up. NULL on a miss, and an expired entry is a miss -- it is reclaimed by the
 * next sc_cache_put that walks the same slot, not by a sweeper.
 *
 * A hit carries one reference for the caller. Release it with sc_cache_release, once, or the
 * entry is never freed.
 */
sc_cache_entry *sc_cache_get(sc_cache *cache, const char *key, size_t key_len);

/**
 * Installs @p value under @p key and takes ownership of it. Insertion never fails for want of
 * space: an occupied slot is overwritten, and among the probe candidates the victim is chosen
 * free slot, then expired, then unused, then the first.
 *
 * Returns an entry holding one reference for the caller, or NULL for a key that is too long or
 * an allocation that failed.
 */
sc_cache_entry *sc_cache_put(sc_cache *cache, const char *key, size_t key_len, void *value);

void *sc_cache_entry_value(const sc_cache_entry *entry);
/** Gives up one reference. Frees the entry when it was the last one. NULL is accepted. */
void sc_cache_release(sc_cache_entry *entry);

#endif /* SERVICE_CORE_CACHE_H */
