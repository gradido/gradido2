/*
 * The request's own memory: an arena that lives exactly as long as the request, parked or not.
 *
 * Both backends call the same five functions at the same five moments, and nothing else about a
 * request's memory differs between them:
 *
 *   loop thread starts      sc_http_arena_thread_begin     the loop's pool is set up
 *   a handler is called     sc_http_arena_enter(req, NULL)
 *   the handler returns     sc_http_arena_leave            freed, unless the request was parked
 *   a request is parked     sc_http_arena_peek / _take     the arena moves into the defer slot
 *   it is resumed           sc_http_arena_enter(req, slot's arena), then _leave again
 *
 * The pool is per loop thread and so is the "current request" -- no lock anywhere, because an
 * arena is only ever returned by the loop that lent it: a parked request comes back to its own
 * loop, and that is where its arena is freed. While it is parked the arena belongs to whoever
 * holds the unit (service_core/db_exec.h), which may allocate from it; the pool is never
 * touched by anybody but its loop.
 */
#ifndef SERVICE_CORE_HTTP_ARENA_H
#define SERVICE_CORE_HTTP_ARENA_H

#include <stdint.h>

#include "service_core/http.h"

void sc_http_arena_thread_begin(uint16_t loop);
void sc_http_arena_thread_end(void);

void sc_http_arena_enter(sc_http_req *req, void *arena);
void sc_http_arena_leave(void);

/** The current request's arena, NULL when it has none yet. Left where it is. */
void *sc_http_arena_peek(void);
/** The current request is being parked: its arena now belongs to the defer slot. */
void sc_http_arena_take(void);

#endif /* SERVICE_CORE_HTTP_ARENA_H */
