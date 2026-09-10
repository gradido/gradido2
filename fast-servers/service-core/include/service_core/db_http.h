/*
 * The executor and the HTTP server, joined: a unit submitted with a request parks the request,
 * runs on a worker, and comes back to answer on the loop the request arrived on.
 *
 *   route      builds the unit in sc_http_alloc memory, then sc_db_submit(exec, req, unit)
 *   executor   parks the request (sc_http_defer) and queues the unit -- or, for a SQLite read,
 *              runs it right there
 *   worker     runs the work, then resumes the request (sc_http_resume)
 *   loop       calls unit->done(unit, req) -- req NULL if the client left -- and the request's
 *              arena, which the unit lives in, is freed when that returns
 *
 * A handler that has submitted must not touch the unit again: once it is queued a worker may
 * be holding it before the handler has even returned.
 */
#ifndef SERVICE_CORE_DB_HTTP_H
#define SERVICE_CORE_DB_HTTP_H

#include "service_core/db_exec.h"
#include "service_core/http.h"
#include "service_core/status.h"

/** Fills the park and finished hooks of @p cfg for @p server. Before sc_db_exec_open. */
void sc_db_http_configure(sc_db_exec_config *cfg, sc_http_server *server);

/** Registers the resume dispatcher on @p server -- one per server, like a route. */
sc_status sc_db_http_attach(sc_http_server *server);

/**
 * Submits @p unit on behalf of @p req, from inside @p req's handler.
 *
 * SC_OK: the unit is on its way, or it already ran and its `done` has been called -- either way
 * the handler returns 0 and answers nothing itself. SC_ERR_QUEUE_FULL: it was not taken, and the
 * handler answers 503 (sc_http_reply_busy). Anything else is an argument error.
 */
sc_status sc_db_submit(sc_db_exec *exec, sc_http_req *req, sc_db_unit *unit);

#endif /* SERVICE_CORE_DB_HTTP_H */
