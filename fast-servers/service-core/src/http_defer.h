/*
 * The table behind sc_http_defer: which requests one loop is currently waiting on.
 *
 * Both backends need exactly this and neither needs it differently, so it lives here rather
 * than twice. What the backends do differently is how a foreign thread wakes the loop -- h2o
 * has h2o_multithread_send_message, the fallback has uv_async_send -- and that stays in them.
 *
 * The rule the whole structure exists to keep: no sc_http_req crosses a thread. A worker holds
 * a ticket, and a ticket is three numbers.
 *
 *   loop        which of the server's loops owns the request      8 bits
 *   slot        where in that loop's table it sits               16 bits
 *   generation  which occupant of that slot it was               30 bits
 *
 * Everything except one word is touched by the loop thread alone. That word is `state`, and it
 * packs the generation with the phase precisely so that a foreign thread can ask "is this
 * ticket still what this slot holds" and claim it in one operation. Checking the generation and
 * then claiming in two steps is a race: a slot released and re-armed in between passes both
 * checks while belonging to somebody else entirely.
 */
#ifndef SERVICE_CORE_HTTP_DEFER_H
#define SERVICE_CORE_HTTP_DEFER_H

#include <stdint.h>

#include "service_core/http.h"

/* The low two bits of `state`. FREE is 0 so a zeroed table is a free table. */
#define SC_DEFER_FREE 0
#define SC_DEFER_ARMED 1
#define SC_DEFER_RESUMING 2

typedef struct sc_defer_slot {
    /* Loop thread only. NULL once the client has gone; the slot stays taken, because a worker
     * is still holding the ticket and something has to hand its work back. */
    sc_http_req *req;
    void *work;             /* loop thread only */
    int32_t next_free;      /* loop thread only; -1 ends the list */
    volatile int32_t state; /* generation << 2 | phase -- the one field two threads share */
} sc_defer_slot;

typedef struct sc_defer_table {
    sc_defer_slot slots[SC_HTTP_DEFER_MAX];
    int32_t free_head; /* loop thread only */
    uint8_t loop_index;
} sc_defer_table;

/** Builds the free list. Startup, before any thread runs. */
void sc_defer_table_init(sc_defer_table *table, uint8_t loop_index);

/**
 * Loop thread. Takes a slot for @p req and returns its ticket, or 0 when the table is full --
 * which is the caller's cue to answer 503 rather than to wait.
 */
sc_http_ticket sc_defer_arm(sc_defer_table *table, sc_http_req *req, void *work);

/**
 * Any thread. Moves exactly the slot @p ticket names from armed to resuming, and answers
 * non-zero when it was this call that moved it. A stale ticket, a slot that has moved on and a
 * second resume of the same ticket all answer zero, which is what makes a double resume an
 * error return instead of a second delivery.
 */
int sc_defer_claim(sc_defer_table *table, sc_http_ticket ticket, int32_t *slot_out);

/**
 * Loop thread. Takes the request and the work out of a resuming slot and gives the slot back.
 * @p req_out is NULL when the client went away while the work was running.
 */
void sc_defer_release(sc_defer_table *table, int32_t slot, sc_http_req **req_out, void **work_out);

/**
 * Loop thread. The client is gone: forget the request but keep the slot, because the worker
 * still holds the ticket and its resume must still arrive somewhere.
 *
 * Does nothing when the slot has since been released and handed to somebody else, which is why
 * the generation is an argument -- the caller learned this from a callback that may fire long
 * after its own request was answered.
 */
void sc_defer_forget(sc_defer_table *table, int32_t slot, uint32_t generation);

/**
 * Loop thread. The next slot at or after @p from whose resume has been claimed but not yet
 * delivered, or -1.
 *
 * The fallback backend walks for these because a uv_async_t coalesces: it says that at least
 * one resume happened and never how many. h2o's queue carries the slot itself and needs none
 * of this.
 */
int32_t sc_defer_next_resuming(const sc_defer_table *table, int32_t from);

/** Which loop, which slot, which occupant -- reading a ticket apart without validating it. */
uint8_t sc_defer_loop_of(sc_http_ticket ticket);
int32_t sc_defer_index_of(sc_http_ticket ticket);
uint32_t sc_defer_generation_of(sc_http_ticket ticket);

#endif /* SERVICE_CORE_HTTP_DEFER_H */
