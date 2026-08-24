/*
 * The HTTP server, as the roles see it.
 *
 * One thin surface, for two reasons. The first is that backend and federation are the same
 * server with different routes and there is no reason to write the accept path twice. The
 * second is Windows: h2o is a posix event loop and does not build against the MSVC runtime, so
 * that build gets a second implementation rather than nothing.
 *
 *   h2o                    the server. HTTP/1.1 and HTTP/2, an answer in 11.6 us
 *   libuv+picohttpparser   what Windows gets instead. One thread, one event loop,
 *                          HTTP/1.1, no TLS -- runnable, and not a deployment option
 *
 * A role does not choose and does not ask. sc_http_backend_name() exists for the startup line
 * and for `--version`, so that what is running is visible rather than inferred.
 *
 * Routes are registered before the loop starts and never after -- which is the whole of the
 * "function pointers at registration only" rule from AGENTS.md, section 1, applied here.
 */
#ifndef SERVICE_CORE_HTTP_H
#define SERVICE_CORE_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "service_core/runtime.h"
#include "service_core/status.h"

typedef struct sc_http_server sc_http_server;
/* Opaque, and it lives exactly as long as the call: everything reachable from it comes out of
 * the request pool, which h2o clears when the response is written. Nothing the answer outlives
 * may be allocated from it. */
typedef struct sc_http_req sc_http_req;

/** Returns 0 to accept the request, -1 to let the next handler try. */
typedef int (*sc_http_handler_fn)(sc_http_req *req, void *user_data);

/*
 * The largest request body either backend accepts, and the same number in both -- a body that
 * one refuses and the other reads would be a difference visible to a client, which is what the
 * integration suite exists to catch. Anything larger is 413.
 */
#define SC_HTTP_MAX_BODY (32 * 1024)

typedef struct sc_http_config {
    const char *host; /* dotted quad; hostnames are not resolved here */
    uint16_t port;
    /* Names the server in its log lines: "backend", "federation". */
    const char *role;
} sc_http_config;

/** Names the backend this build linked: "h2o" or "libuv+picohttpparser". Never NULL. */
const char *sc_http_backend_name(void);

sc_http_server *sc_http_server_create(const sc_http_config *cfg);
void sc_http_server_destroy(sc_http_server *server);

/**
 * Registers @p fn for exactly @p path, query string excluded from the match. Startup only --
 * see the file comment. @p path is borrowed and must outlive the server, which a string literal
 * does.
 *
 * A handler returns 0 when it answered and -1 to pass the request to the next route registered
 * for the same path. Nothing answering it is a 404.
 */
sc_status sc_http_route(sc_http_server *server, const char *path, sc_http_handler_fn fn,
                        void *user_data);

/** Binds and listens. Separate from the run loop so a failure to take the port is reported
 *  before any thread starts serving. */
sc_status sc_http_listen(sc_http_server *server);

/** Runs until @p quit is raised. Returns SC_OK on a clean shutdown. */
sc_status sc_http_run(sc_http_server *server, const sc_quit_flag *quit);

/* --- inside a handler ------------------------------------------------------------------ */

/*
 * Everything below borrows from the request and is valid until the handler returns. Nothing is
 * NUL-terminated: a length comes back through the out parameter, because a header value and a
 * body may both contain a zero byte, and a strlen would be a truncation an attacker chooses.
 */

/** Case-sensitive compare against the request method, e.g. "GET". */
int sc_http_method_is(const sc_http_req *req, const char *method);

/** The request path, query string excluded. Never NULL for a request that reached a handler. */
const char *sc_http_path(const sc_http_req *req, size_t *len);

/** The complete request body, or NULL when there is none. @p len is set either way. */
const char *sc_http_body(const sc_http_req *req, size_t *len);

/** Case-insensitive header lookup. NULL when absent; @p len may be NULL. */
const char *sc_http_header(const sc_http_req *req, const char *name, size_t *len);

/** 0 for HTTP/1.0, 1 for HTTP/1.1. HTTP/2 answers 1 -- what is being asked about is the
 *  semantics a client expects, not the framing underneath. */
int sc_http_minor_version(const sc_http_req *req);

/**
 * Sends @p body as the complete response. @p body must outlive the call only until it returns;
 * a string literal is the intended case.
 */
sc_status sc_http_reply(sc_http_req *req, int status, const char *content_type, const char *body,
                        size_t body_len);

/**
 * Liveness, and deliberately nothing more: it reports that the process is up and which role
 * answered, reads no database and touches no session.
 *
 * It is an operational endpoint, not business behavior, which is why it is not in
 * contracts/server/ and why it does not violate "no feature originates in the fast path" --
 * there is nothing here for the TypeScript path to be missing. @p user_data is the role name.
 */
int sc_http_health(sc_http_req *req, void *user_data);

#define SC_HTTP_HEALTH_PATH "/_health"

#endif /* SERVICE_CORE_HTTP_H */
