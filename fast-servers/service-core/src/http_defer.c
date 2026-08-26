#include "http_defer.h"

#include <stddef.h>

#include "service_core/atomic.h"

/*
 * Ticket layout. The generation gets what is left after the loop and the slot, which is thirty
 * bits: a slot would have to be handed out a billion times before a ticket could be mistaken
 * for an older one, and a slot is only reused after its request has been answered.
 */
#define TICKET_LOOP_SHIFT 56
#define TICKET_SLOT_SHIFT 32
#define TICKET_SLOT_MASK 0xffffu
#define TICKET_GEN_MASK 0x3fffffffu

static sc_http_ticket ticket_of(uint8_t loop, int32_t slot, uint32_t generation)
{
    return ((sc_http_ticket)loop << TICKET_LOOP_SHIFT) |
           ((sc_http_ticket)((uint32_t)slot & TICKET_SLOT_MASK) << TICKET_SLOT_SHIFT) |
           (sc_http_ticket)(generation & TICKET_GEN_MASK);
}

uint8_t sc_defer_loop_of(sc_http_ticket ticket)
{
    return (uint8_t)(ticket >> TICKET_LOOP_SHIFT);
}

int32_t sc_defer_index_of(sc_http_ticket ticket)
{
    return (int32_t)((ticket >> TICKET_SLOT_SHIFT) & TICKET_SLOT_MASK);
}

uint32_t sc_defer_generation_of(sc_http_ticket ticket)
{
    return (uint32_t)(ticket & TICKET_GEN_MASK);
}

static int32_t state_of(uint32_t generation, int32_t phase)
{
    /* The cast is to a bit pattern, not to a number: the state word is only ever compared and
     * swapped, never ordered, so a generation with its top bit set is not a negative anything. */
    return (int32_t)(((generation & TICKET_GEN_MASK) << 2) | (uint32_t)phase);
}

static uint32_t generation_in(int32_t state)
{
    return ((uint32_t)state >> 2) & TICKET_GEN_MASK;
}

static int32_t phase_in(int32_t state)
{
    return state & 3;
}

void sc_defer_table_init(sc_defer_table *table, uint8_t loop_index)
{
    int32_t i;

    table->loop_index = loop_index;
    table->free_head = 0;
    for (i = 0; i != SC_HTTP_DEFER_MAX; ++i) {
        table->slots[i].req = NULL;
        table->slots[i].work = NULL;
        table->slots[i].next_free = i + 1 == SC_HTTP_DEFER_MAX ? -1 : i + 1;
        /* Generation 0, phase free. The first arm makes it 1, which is what keeps a real
         * ticket from ever being zero: loop 0, slot 0, generation 0 would be. */
        table->slots[i].state = state_of(0, SC_DEFER_FREE);
    }
}

sc_http_ticket sc_defer_arm(sc_defer_table *table, sc_http_req *req, void *work)
{
    int32_t index = table->free_head;
    sc_defer_slot *slot;
    uint32_t generation;

    if (index < 0)
        return 0;
    slot = &table->slots[index];
    table->free_head = slot->next_free;
    slot->next_free = -1;
    slot->req = req;
    slot->work = work;

    /* An atomic load even though this slot is free and no claim can succeed against it: a stale
     * ticket may still attempt the compare and swap, and a plain read racing a failed atomic
     * read-modify-write is a data race whatever the outcome would have been. */
    generation = generation_in(sc_atomic_load(&slot->state)) + 1;
    if ((generation & TICKET_GEN_MASK) == 0)
        generation = 1; /* skip zero on the wrap, so the ticket stays non-zero */

    /* Release: req and work are written before any thread can see the armed phase. */
    sc_atomic_store(&slot->state, state_of(generation, SC_DEFER_ARMED));
    return ticket_of(table->loop_index, index, generation);
}

int sc_defer_claim(sc_defer_table *table, sc_http_ticket ticket, int32_t *slot_out)
{
    int32_t index = sc_defer_index_of(ticket);
    uint32_t generation = sc_defer_generation_of(ticket);

    if (ticket == 0 || sc_defer_loop_of(ticket) != table->loop_index)
        return 0;
    if (index < 0 || index >= SC_HTTP_DEFER_MAX)
        return 0;
    if (!sc_atomic_cas(&table->slots[index].state, state_of(generation, SC_DEFER_ARMED),
                       state_of(generation, SC_DEFER_RESUMING)))
        return 0;
    *slot_out = index;
    return 1;
}

void sc_defer_release(sc_defer_table *table, int32_t slot, sc_http_req **req_out, void **work_out)
{
    sc_defer_slot *entry = &table->slots[slot];

    *req_out = entry->req;
    *work_out = entry->work;
    entry->req = NULL;
    entry->work = NULL;

    /* The generation is not bumped here but at the next arm. Between the two the slot holds
     * its old generation with the free phase, and a stale ticket asking for the armed phase
     * fails on the phase rather than on the number. */
    sc_atomic_store(&entry->state,
                    state_of(generation_in(sc_atomic_load(&entry->state)), SC_DEFER_FREE));
    entry->next_free = table->free_head;
    table->free_head = slot;
}

void sc_defer_forget(sc_defer_table *table, int32_t slot, uint32_t generation)
{
    sc_defer_slot *entry;
    int32_t state;

    if (slot < 0 || slot >= SC_HTTP_DEFER_MAX)
        return;
    entry = &table->slots[slot];
    state = sc_atomic_load(&entry->state);
    if (phase_in(state) == SC_DEFER_FREE || generation_in(state) != (generation & TICKET_GEN_MASK))
        return; /* answered already, and the slot belongs to somebody else now */
    entry->req = NULL;
}

int32_t sc_defer_next_resuming(const sc_defer_table *table, int32_t from)
{
    int32_t i;

    for (i = from < 0 ? 0 : from; i != SC_HTTP_DEFER_MAX; ++i) {
        if (phase_in(sc_atomic_load(&table->slots[i].state)) == SC_DEFER_RESUMING)
            return i;
    }
    return -1;
}
