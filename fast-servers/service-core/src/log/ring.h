/*
 * The ring between the submitting threads and the drain thread, and the arena pools that feed
 * it.
 *
 * How the state is divided, because everything else follows from it:
 *
 *   per submitting thread   one arnm_graded_arena_pool and one return queue. Only that
 *                           thread and the drain thread touch either, and the drain thread
 *                           only writes at the head of the return queue. The pool itself is
 *                           never touched by two threads -- that is arnm's condition, verbatim.
 *   shared                  one MPSC ring of {arena*, record*, owner}. Fixed slot size,
 *                           because the payload lives in the arena and not in the ring.
 *   per drain thread        the output buffers, over in logger.c. One writer, no lock.
 *
 * A Vyukov ring: a sequence word per slot separates "reserved" from "fully written", which two
 * cursors on their own cannot tell apart -- the consumer would read a slot whose producer is
 * still mid-write. The cursors and the sequence protocol stay inside ring.c; what leaves
 * this header is the handful of moves that keep the discipline intact.
 *
 * Producer:  slot = sc_log_ring_reserve(&pos) -> fill it -> sc_log_ring_publish(pos)
 * Consumer:  sc_log_ring_take(&slot, flush)   -> read it -> sc_log_ring_done(slot)
 *
 * sc_log_ring_reserve() waits when the slot it drew is still in use. That wait is the
 * backpressure and it is unbounded on purpose: the index is monotonic and the slot will come
 * free, so a submitter waits rather than drops. What bounds it is the sink's stall clock.
 */
#ifndef SERVICE_CORE_LOG_RING_H
#define SERVICE_CORE_LOG_RING_H

#include <stdatomic.h>
#include <stdint.h>

#include "arnm/graded_arena_pool.h"
#include "record.h"

typedef struct sc_log_slot {
    _Atomic uint64_t seq;
    arnm *arena;
    sc_log_record *rec;
    uint32_t owner;
} sc_log_slot;

typedef struct sc_log_owner {
    arnm_graded_arena_pool pool;
    /* Returns: only the logger writes at the head, only the owner reads at the tail. */
    arnm **ret;
    uint32_t ret_mask;
    _Atomic uint64_t ret_head;
    _Atomic uint64_t ret_tail;
    _Atomic int in_use;
    uint64_t host_allocs;
    char pad[64];
} sc_log_owner;

/* --- the ring itself, set up and taken down by sc_log_init() and sc_log_shutdown() --- */

/** Rounds @p requested up to a power of two and reserves that many slots. 0 on success. */
int sc_log_ring_init(uint32_t requested);
/** Gives the slots back. The logger thread must already have been joined. */
void sc_log_ring_free(void);
/** Tells the logger to stop and wakes everyone waiting on the ring. */
void sc_log_ring_stop(void);
uint64_t sc_log_ring_capacity(void);
/** Slots taken but not yet drained, for the high-water warning. */
uint64_t sc_log_ring_fill(void);

/* --- producer --- */

/** A ticket and the slot it names, waiting out the backpressure. Never fails. */
sc_log_slot *sc_log_ring_reserve(uint64_t *out_pos);
/** The slot is written: hand it over and wake the logger if it is asleep. */
void sc_log_ring_publish(uint64_t pos);

/* --- consumer, the drain thread alone --- */

/*
 * The next line in ticket order. Spins, then sleeps, while the ring is empty, calling
 * @p before_sleep once before it actually sleeps -- the drain thread puts out what it is
 * holding there, so a quiet moment does not leave a line sitting in a buffer.
 * Answers 0 once the ring is empty and sc_log_shutdown() has been asked for.
 */
int sc_log_ring_take(sc_log_slot **out, void (*before_sleep)(void));
/** The line is written out: free the slot and wake whoever is waiting for it. */
void sc_log_ring_done(sc_log_slot *slot);

/* --- owners: one arena pool and one return queue per submitting thread --- */

/** Claims a free owner slot for this thread and prefills its pool. The index, or -1. */
int sc_log_owner_join(void);
/** Waits for every arena of this thread to come back, then gives the pool up. */
void sc_log_owner_leave(void);
/** This thread's owner index, or -1 if it has not joined. */
int sc_log_owner_index(void);
sc_log_owner *sc_log_owner_at(int index);
/** The logger hands a spent arena back to its owner. Only the owner ever frees it. */
void sc_log_owner_return(sc_log_owner *o, arnm *arena);
/** The owner collects what came back. No lock: the queue has one writer and one reader. */
void sc_log_owner_reclaim(sc_log_owner *o);
uint64_t sc_log_owner_host_allocs_total(void);

#endif /* SERVICE_CORE_LOG_RING_H */
