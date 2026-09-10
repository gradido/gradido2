/*
 * The request arena -- see http_arena.h for when each function is called, and http.h,
 * sc_http_alloc, for what a handler sees.
 */
#include "http_arena.h"

#include <stddef.h>

#include "arnm/arena.h"
#include "arnm/graded_arena_pool.h"
#include "arnm/memory.h"

/*
 * The sizes a request's arena comes in. A registration needs a few hundred bytes and gets the
 * smallest; the largest is the ceiling on what one request may hold at all, parked data and
 * results included -- a response that could grow past it has to be paged, which is the answer
 * a high-load server wants anyway.
 */
static const uint32_t kGrades[] = {16u * 1024u, 64u * 1024u, 256u * 1024u, 1024u * 1024u};
#define GRADE_COUNT ((uint16_t)(sizeof(kGrades) / sizeof(kGrades[0])))
/* Arenas each grade keeps in stock once they have been used: a loop that served a burst keeps
 * enough to serve the next one without asking the host. */
#define SPARE_PER_GRADE 16u

static _Thread_local arnm_graded_arena_pool t_pool;
static _Thread_local int t_pool_ready;
static _Thread_local uint16_t t_loop;
static _Thread_local sc_http_req *t_req;
static _Thread_local arnm *t_arena;
static _Thread_local int t_parked;

void sc_http_arena_thread_begin(uint16_t loop)
{
    t_loop = loop;
    t_req = NULL;
    t_arena = NULL;
    t_parked = 0;
    if (!t_pool_ready)
        t_pool_ready = arnm_graded_arena_pool_init(&t_pool, kGrades, GRADE_COUNT,
                                                   SPARE_PER_GRADE) == ARNM_SUCCESS;
}

void sc_http_arena_thread_end(void)
{
    if (t_pool_ready) {
        (void)arnm_graded_arena_pool_release(&t_pool);
        t_pool_ready = 0;
    }
}

void sc_http_arena_enter(sc_http_req *req, void *arena)
{
    t_req = req;
    t_arena = (arnm *)arena;
    t_parked = 0;
}

void sc_http_arena_leave(void)
{
    if (t_arena != NULL && !t_parked && t_pool_ready)
        (void)arnm_graded_arena_pool_free(&t_pool, t_arena);
    t_req = NULL;
    t_arena = NULL;
    t_parked = 0;
}

void *sc_http_arena_peek(void)
{
    return t_arena;
}

void sc_http_arena_take(void)
{
    t_parked = 1;
    t_arena = NULL;
}

uint16_t sc_http_current_loop(void)
{
    return t_loop;
}

static arnm *arena_for(sc_http_req *req, uint32_t capacity)
{
    /* A parked request's arena belongs to whoever holds its work; the handler that parked it
     * gets nothing more, rather than a second arena nobody would ever give back. */
    if (req == NULL || req != t_req || !t_pool_ready || t_parked)
        return NULL;
    if (t_arena == NULL &&
        arnm_graded_arena_pool_alloc(&t_pool, capacity, &t_arena) != ARNM_SUCCESS)
        t_arena = NULL;
    return t_arena;
}

sc_status sc_http_reserve(sc_http_req *req, size_t capacity)
{
    if (capacity > kGrades[GRADE_COUNT - 1])
        return SC_ERR_TOO_LONG;
    if (req == NULL || req != t_req || t_arena != NULL)
        return SC_ERR_INVALID_ARGUMENT;
    return arena_for(req, (uint32_t)capacity) != NULL ? SC_OK : SC_ERR_NO_MEMORY;
}

void *sc_http_alloc(sc_http_req *req, size_t size)
{
    arnm *arena;
    uint8_t *out = NULL;

    if (size == 0 || size > kGrades[GRADE_COUNT - 1])
        return NULL;
    arena = arena_for(req, (uint32_t)size < kGrades[0] ? kGrades[0] : (uint32_t)size);
    if (arena == NULL || arnm_alloc(&out, (uint32_t)size, arena) != ARNM_SUCCESS)
        return NULL;
    return out;
}

void *sc_http_request_arena(sc_http_req *req)
{
    return arena_for(req, kGrades[0]);
}
