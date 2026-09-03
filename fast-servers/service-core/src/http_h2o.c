/*
 * The h2o backend: one event loop per thread, the evloop socket layer, no libuv loop.
 *
 * The accept path follows h2o's own examples and the h2o prototype. Two things are different
 * here and both are deliberate:
 *
 *   the loop is not `while (h2o_evloop_run(loop, INT32_MAX))`. It ticks, so the quit flag a
 *   signal handler raised is seen within SC_RUNTIME_TICK_MS and every role shuts down together.
 *
 *   there are several of them. h2o is thread-per-loop -- one globalconf shared read-only, one
 *   h2o_context_t per thread, and an h2o_req_t that belongs to exactly one of them and is never
 *   handed to another. Each loop takes its own listening socket under SO_REUSEPORT and the
 *   kernel does the balancing, which is what removes the accept lock and the thundering herd.
 *   Architecture.md, *Threading*, holds why the count is the number of cores and not more.
 *
 * Idioms worth not rediscovering are recorded in AGENTS.md section 6, not here.
 */
#include "service_core/http.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <uv.h>

#include "h2o.h"
#include "h2o/http1.h"

#include "http_defer.h"
#include "service_core/log.h"

/*
 * h2o allocates the handler and hands it back on every request, so the registration data rides
 * along inside it. This is the one place a function pointer is stored, and it is stored at
 * registration -- AGENTS.md section 1.
 */
typedef struct {
    h2o_handler_t super;
    sc_http_handler_fn fn;
    void *user_data;
    /* So that on_request can reach the before-route hook, which is per server and not per
     * route. h2o hands back the handler and nothing else. */
    struct sc_http_server *server;
} sc_route_handler;

/*
 * One of these per thread. Everything h2o keeps per context is in here, and so is the deferred
 * request table -- which is per loop rather than per server precisely so that resolving a
 * ticket needs no lock: only this thread ever adds to it or takes from it.
 */
typedef struct sc_http_loop {
    h2o_context_t context;
    h2o_accept_ctx_t accept_ctx;
    h2o_multithread_queue_t *queue;
    h2o_multithread_receiver_t receiver;
    /* One message per slot, so a resume allocates nothing. A slot is resumed once, so its
     * message is never on the queue twice. */
    h2o_multithread_message_t messages[SC_HTTP_DEFER_MAX];
    sc_defer_table defer;
    struct sc_http_server *server;
    uv_thread_t thread;
    int listen_fd;
    int context_started;
    int thread_started;
    sc_status status;
} sc_http_loop;

struct sc_http_server {
    h2o_globalconf_t config;
    h2o_hostconf_t *hostconf;
    /* Allocated at create, never resized. Not a fixed array: SC_HTTP_THREADS_MAX of these is
     * megabytes, and this process is one whose resident size is a feature. */
    sc_http_loop *loops;
    uint16_t thread_count;
    const sc_quit_flag *quit;
    sc_http_resume_fn on_resume;
    void *on_resume_data;
    sc_http_handler_fn before_route;
    void *before_route_data;
    char host[64];
    char role[32];
    uint16_t port;
};

/* The generator is how a deferred request learns that its client left: h2o calls `stop` when it
 * disposes a request whose response never started, and clears the generator itself on a final
 * send, so `stop` fires exactly when the answer did not. It is allocated from the request pool
 * -- nothing on the request path mallocs -- and carries the ticket rather than a slot pointer,
 * because by the time it fires the slot may belong to somebody else. */
typedef struct {
    h2o_generator_t super;
    sc_http_loop *loop;
    int32_t slot;
    uint32_t generation;
} sc_defer_generator;

/* Defined below, beside the rest of the deferred-request machinery; registered up in
 * sc_http_listen, which is where a loop is built. */
static void on_resume_message(h2o_multithread_receiver_t *receiver, h2o_linklist_t *messages);

const char *sc_http_backend_name(void)
{
    return "h2o";
}

static int on_request(h2o_handler_t *self, h2o_req_t *req)
{
    sc_route_handler *route = (sc_route_handler *)self;

    /* What applies to every request, before the one that applies to this path. A hook that
     * answers has answered; nothing else runs. */
    if (route->server->before_route != NULL &&
        route->server->before_route((sc_http_req *)req, route->server->before_route_data) == 0)
        return 0;
    return route->fn((sc_http_req *)req, route->user_data);
}

static uint16_t resolve_threads(uint16_t requested)
{
    unsigned int cores;

    if (requested != 0)
        return requested > SC_HTTP_THREADS_MAX ? SC_HTTP_THREADS_MAX : requested;
    cores = uv_available_parallelism();
    if (cores == 0)
        return 1;
    return cores > SC_HTTP_THREADS_MAX ? SC_HTTP_THREADS_MAX : (uint16_t)cores;
}

sc_http_server *sc_http_server_create(const sc_http_config *cfg)
{
    sc_http_server *server;
    uint16_t i;

    if (cfg == NULL || cfg->host == NULL || cfg->role == NULL)
        return NULL;
    if (strlen(cfg->host) >= sizeof(server->host) || strlen(cfg->role) >= sizeof(server->role))
        return NULL;

    server = (sc_http_server *)calloc(1, sizeof(*server));
    if (server == NULL)
        return NULL;
    server->port = cfg->port;
    memcpy(server->host, cfg->host, strlen(cfg->host) + 1);
    memcpy(server->role, cfg->role, strlen(cfg->role) + 1);

    server->thread_count = resolve_threads(cfg->threads);
    server->loops = (sc_http_loop *)calloc(server->thread_count, sizeof(*server->loops));
    if (server->loops == NULL) {
        free(server);
        return NULL;
    }
    for (i = 0; i != server->thread_count; ++i) {
        server->loops[i].server = server;
        server->loops[i].listen_fd = -1;
        server->loops[i].status = SC_OK;
        sc_defer_table_init(&server->loops[i].defer, (uint8_t)i);
    }

    h2o_config_init(&server->config);
    /* h2o's own default is a gigabyte. The fallback backend stops at SC_HTTP_MAX_BODY, and a
     * body one accepts while the other answers 413 is a difference a client can see. */
    server->config.max_request_entity_size = SC_HTTP_MAX_BODY;
    /*
     * No `Server:` header. h2o defaults it to "h2o/" H2O_VERSION and emits it on every response,
     * error pages included -- and because this build compiles h2o from a git checkout rather
     * than a release, what it announced was "h2o/2.3.0-DEV": not merely the server, but an
     * unreleased build of it.
     *
     * This is not a security control. Anyone looking will still recognise h2o from the order of
     * its headers and the shape of its HTTP/2 settings, and hiding a version has never fixed a
     * vulnerability. What it denies is the cheap pass: a scanner that shortlists hosts by banner
     * for a known CVE gets nothing, and that costs one assignment.
     *
     * A zero length name is the supported way -- lib/http1.c writes the header only when
     * `server_name.len` is non-zero. The fallback backend has never sent one, which is what
     * makes this a difference between the two rather than a preference; tests/integration
     * asserts that neither does.
     */
    server->config.server_name = h2o_iovec_init(NULL, 0);
    server->hostconf =
        h2o_config_register_host(&server->config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    return server;
}

void sc_http_server_destroy(sc_http_server *server)
{
    uint16_t i;

    if (server == NULL)
        return;
    for (i = 0; i != server->thread_count; ++i) {
        sc_http_loop *loop = &server->loops[i];
        /* The queue owns a socket on this loop, so it goes before the context that owns the
         * loop. Nothing is running by now: sc_http_run joined every thread before returning. */
        if (loop->queue != NULL) {
            h2o_multithread_unregister_receiver(loop->queue, &loop->receiver);
            h2o_multithread_destroy_queue(loop->queue);
        }
        if (loop->context_started) {
            h2o_loop_t *evloop = loop->context.loop;
            h2o_context_dispose(&loop->context);
            h2o_evloop_destroy(evloop);
        }
        if (loop->listen_fd != -1)
            (void)close(loop->listen_fd);
    }
    free(server->loops);
    h2o_config_dispose(&server->config);
    free(server);
}

uint16_t sc_http_thread_count(const sc_http_server *server)
{
    return server != NULL ? server->thread_count : 0;
}

sc_status sc_http_route(sc_http_server *server, const char *path, sc_http_handler_fn fn,
                        void *user_data)
{
    h2o_pathconf_t *pathconf;
    sc_route_handler *handler;

    if (server == NULL || path == NULL || fn == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    pathconf = h2o_config_register_path(server->hostconf, path, 0);
    handler = (sc_route_handler *)h2o_create_handler(pathconf, sizeof(*handler));
    if (handler == NULL)
        return SC_ERR_NO_MEMORY;
    handler->super.on_req = on_request;
    handler->fn = fn;
    handler->user_data = user_data;
    handler->server = server;
    return SC_OK;
}

sc_status sc_http_route_default(sc_http_server *server, sc_http_handler_fn fn, void *user_data)
{
    sc_route_handler *handler;

    if (server == NULL || fn == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    /* h2o already has the concept: hostconf->fallback_path is the pathconf a request is bound
     * to when no registered path matched, and it is where h2o's own 404 comes from. Registering
     * here rather than at "/" is what keeps this a fallback instead of a prefix -- a handler at
     * "/" would also be consulted for a path that a longer route matched. */
    handler = (sc_route_handler *)h2o_create_handler(&server->hostconf->fallback_path,
                                                     sizeof(*handler));
    if (handler == NULL)
        return SC_ERR_NO_MEMORY;
    handler->super.on_req = on_request;
    handler->fn = fn;
    handler->user_data = user_data;
    handler->server = server;
    return SC_OK;
}

sc_status sc_http_before_route(sc_http_server *server, sc_http_handler_fn fn, void *user_data)
{
    if (server == NULL || fn == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    server->before_route = fn;
    server->before_route_data = user_data;
    return SC_OK;
}

sc_status sc_http_on_resume(sc_http_server *server, sc_http_resume_fn fn, void *user_data)
{
    if (server == NULL || fn == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    server->on_resume = fn;
    server->on_resume_data = user_data;
    return SC_OK;
}

static void on_accept(h2o_socket_t *listener, const char *err)
{
    sc_http_loop *loop = (sc_http_loop *)listener->data;
    h2o_socket_t *sock;

    if (err != NULL)
        return;
    if ((sock = h2o_evloop_socket_accept(listener)) == NULL)
        return;
    h2o_accept(&loop->accept_ctx, sock);
}

/**
 * One listening socket per loop, each with SO_REUSEPORT: the kernel spreads connections over
 * them. Sharing one socket instead would put every loop on one accept queue, which is the
 * thundering herd this arrangement exists to avoid.
 *
 * The spreading is Linux's, and this is the one place the fast path assumes the machine it is
 * deployed on. Darwin lets the sockets bind and hands new connections to the last one bound
 * rather than distributing them, so a macOS build serves correctly on one loop while the others
 * idle. That is a development machine, and Architecture.md, *Windows*, already says where this
 * server runs.
 */
static sc_status bind_one(sc_http_server *server, sc_http_loop *loop,
                          const struct sockaddr_in *addr)
{
    int reuse = 1;

    loop->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (loop->listen_fd == -1 ||
        setsockopt(loop->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        setsockopt(loop->listen_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) != 0 ||
        bind(loop->listen_fd, (const struct sockaddr *)addr, sizeof(*addr)) != 0 ||
        listen(loop->listen_fd, SOMAXCONN) != 0) {
        sc_log_error(SC_CAT_STARTUP, "server.listen.failed", "%s cannot listen on %s:%u: %s",
                     server->role, server->host, (unsigned)server->port, strerror(errno));
        return SC_ERR_NETWORK;
    }
    return SC_OK;
}

sc_status sc_http_listen(sc_http_server *server)
{
    struct sockaddr_in addr;
    uint16_t i;

    if (server == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server->port);
    if (inet_pton(AF_INET, server->host, &addr.sin_addr) != 1) {
        sc_log_error(SC_CAT_STARTUP, "server.listen.host_invalid",
                     "%s cannot listen on %s: not an IPv4 address", server->role, server->host);
        return SC_ERR_INVALID_ARGUMENT;
    }

    /* Every socket is bound here, on the calling thread, before any of them serves. A port
     * already taken is then one error line at startup rather than N of them from N threads. */
    for (i = 0; i != server->thread_count; ++i) {
        sc_http_loop *loop = &server->loops[i];
        h2o_socket_t *sock;
        sc_status status = bind_one(server, loop, &addr);

        if (status != SC_OK)
            return status;

        /* The context owns the loop; both are created here rather than in _create so that a
         * server that never got its port does not leave event loops behind. */
        h2o_context_init(&loop->context, h2o_evloop_create(), &server->config);
        loop->context_started = 1;
        loop->accept_ctx.ctx = &loop->context;
        loop->accept_ctx.hosts = server->config.hosts;

        loop->queue = h2o_multithread_create_queue(loop->context.loop);
        if (loop->queue == NULL)
            return SC_ERR_NO_MEMORY;
        h2o_multithread_register_receiver(loop->queue, &loop->receiver, on_resume_message);

        sock = h2o_evloop_socket_create(loop->context.loop, loop->listen_fd,
                                        H2O_SOCKET_FLAG_DONT_READ);
        if (sock == NULL)
            return SC_ERR_NETWORK;
        sock->data = loop;
        h2o_socket_read_start(sock, on_accept);
    }

    sc_log_info(SC_CAT_STARTUP, "server.listen", "%s listening on http://%s:%u, %u thread%s",
                server->role, server->host, (unsigned)server->port, (unsigned)server->thread_count,
                server->thread_count == 1 ? "" : "s");
    return SC_OK;
}

static void run_loop(void *arg)
{
    sc_http_loop *loop = (sc_http_loop *)arg;

    while (!sc_quit_requested(loop->server->quit)) {
        if (h2o_evloop_run(loop->context.loop, SC_RUNTIME_TICK_MS) != 0) {
            loop->status = SC_ERR_NETWORK;
            return;
        }
    }
}

sc_status sc_http_run(sc_http_server *server, const sc_quit_flag *quit)
{
    sc_status status = SC_OK;
    uint16_t i;

    if (server == NULL || quit == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    server->quit = quit;

    /* Loop 0 runs on the calling thread. A role therefore still owns exactly the one thread
     * main gave it, and the others are this function's own business from start to join. */
    for (i = 1; i != server->thread_count; ++i) {
        if (uv_thread_create(&server->loops[i].thread, run_loop, &server->loops[i]) != 0) {
            sc_log_error(SC_CAT_STARTUP, "server.thread.failed", "%s could not start loop %u of %u",
                         server->role, (unsigned)i, (unsigned)server->thread_count);
            /* The threads that did start are joined below; raising the flag is what brings
             * them back. A server serving on fewer loops than it was asked for is not a
             * degraded mode this offers -- it is a startup failure. */
            sc_runtime_request_quit();
            status = SC_ERR_NETWORK;
            break;
        }
        server->loops[i].thread_started = 1;
    }

    if (status == SC_OK)
        run_loop(&server->loops[0]);

    for (i = 1; i != server->thread_count; ++i) {
        if (!server->loops[i].thread_started)
            continue;
        (void)uv_thread_join(&server->loops[i].thread);
    }
    for (i = 0; i != server->thread_count && status == SC_OK; ++i)
        status = server->loops[i].status;

    sc_log_info(SC_CAT_STARTUP, "server.stop", "%s stopped", server->role);
    return status;
}

/* --- answering later -------------------------------------------------------------------- */

static void deliver(sc_http_loop *loop, int32_t slot)
{
    sc_http_req *req = NULL;
    void *work = NULL;

    /* The slot goes back before the callback runs, so a callback that defers again -- or one
     * that takes its time -- does not hold a slot it no longer needs. Both values were copied
     * out, so nothing here reads the slot afterwards. */
    sc_defer_release(&loop->defer, slot, &req, &work);
    loop->server->on_resume(req, work, loop->server->on_resume_data);
}

static void on_resume_message(h2o_multithread_receiver_t *receiver, h2o_linklist_t *messages)
{
    sc_http_loop *loop = H2O_STRUCT_FROM_MEMBER(sc_http_loop, receiver, receiver);

    while (!h2o_linklist_is_empty(messages)) {
        h2o_multithread_message_t *message =
            H2O_STRUCT_FROM_MEMBER(h2o_multithread_message_t, link, messages->next);
        /* Which slot this is, is where the message sits in the array. One message per slot is
         * what keeps a resume from allocating. */
        int32_t slot = (int32_t)(message - loop->messages);

        h2o_linklist_unlink(&message->link);
        deliver(loop, slot);
    }
}

static void on_defer_stop(h2o_generator_t *super, h2o_req_t *req)
{
    sc_defer_generator *generator = (sc_defer_generator *)super;
    (void)req;

    /* The client is gone. The slot stays taken -- a worker is still holding its ticket and its
     * resume has to land somewhere -- and only the request is forgotten. */
    sc_defer_forget(&generator->loop->defer, generator->slot, generator->generation);
}

static sc_http_loop *loop_of(sc_http_server *server, const h2o_req_t *req)
{
    uint16_t i;

    /* A linear walk over as many contexts as the machine has cores, on the path where
     * something is about to wait for a database. It costs nothing worth a second structure. */
    for (i = 0; i != server->thread_count; ++i) {
        if (req->conn->ctx == &server->loops[i].context)
            return &server->loops[i];
    }
    return NULL;
}

sc_status sc_http_defer(sc_http_server *server, sc_http_req *req, void *work, sc_http_ticket *out)
{
    h2o_req_t *r = (h2o_req_t *)req;
    sc_defer_generator *generator;
    sc_http_loop *loop;
    sc_http_ticket ticket;

    if (server == NULL || r == NULL || out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (server->on_resume == NULL)
        return SC_ERR_UNAVAILABLE;
    if ((loop = loop_of(server, r)) == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    ticket = sc_defer_arm(&loop->defer, req, work);
    if (ticket == 0)
        return SC_ERR_QUEUE_FULL;

    /* The generator is registered before the ticket leaves this function, which is the whole of
     * the rule in AGENTS.md section 6: a client that disconnects while the work runs must find
     * a `stop` callback already in place, or it writes into a request nobody owns any more. */
    generator = h2o_mem_alloc_pool(&r->pool, sc_defer_generator, 1);
    generator->super.proceed = NULL;
    generator->super.stop = on_defer_stop;
    generator->loop = loop;
    generator->slot = sc_defer_index_of(ticket);
    generator->generation = sc_defer_generation_of(ticket);
    /* This is what makes the request outlive the handler. It snapshots res.status into
     * res.original for the access loggers, of which this server registers none. */
    h2o_start_response(r, &generator->super);

    *out = ticket;
    return SC_OK;
}

sc_status sc_http_resume(sc_http_server *server, sc_http_ticket ticket)
{
    sc_http_loop *loop;
    uint8_t index;
    int32_t slot;

    if (server == NULL || ticket == 0)
        return SC_ERR_INVALID_ARGUMENT;
    index = sc_defer_loop_of(ticket);
    if (index >= server->thread_count)
        return SC_ERR_INVALID_ARGUMENT;
    loop = &server->loops[index];

    if (!sc_defer_claim(&loop->defer, ticket, &slot))
        return SC_ERR_UNAVAILABLE;
    h2o_multithread_send_message(&loop->receiver, &loop->messages[slot]);
    return SC_OK;
}

/* --- inside a handler ------------------------------------------------------------------- */

int sc_http_method_is(const sc_http_req *req, const char *method)
{
    const h2o_req_t *r = (const h2o_req_t *)req;
    size_t len;

    if (r == NULL || method == NULL)
        return 0;
    len = strlen(method);
    return r->method.len == len && memcmp(r->method.base, method, len) == 0;
}

const char *sc_http_path(const sc_http_req *req, size_t *len)
{
    const h2o_req_t *r = (const h2o_req_t *)req;

    if (r == NULL) {
        if (len != NULL)
            *len = 0;
        return NULL;
    }
    /* path_normalized is the path with the query string already split off and the escapes
     * resolved, which is what a route is matched against. */
    if (len != NULL)
        *len = r->path_normalized.len;
    return r->path_normalized.base;
}

const char *sc_http_body(const sc_http_req *req, size_t *len)
{
    const h2o_req_t *r = (const h2o_req_t *)req;

    /* h2o has read the whole entity before calling the handler, so this never waits. */
    if (r == NULL || r->entity.len == 0) {
        if (len != NULL)
            *len = 0;
        return NULL;
    }
    if (len != NULL)
        *len = r->entity.len;
    return r->entity.base;
}

const char *sc_http_header(const sc_http_req *req, const char *name, size_t *len)
{
    const h2o_req_t *r = (const h2o_req_t *)req;
    size_t name_len;
    size_t i;

    if (r == NULL || name == NULL)
        return NULL;
    name_len = strlen(name);
    for (i = 0; i != r->headers.size; ++i) {
        const h2o_header_t *header = &r->headers.entries[i];
        /* h2o lowercases what it parses, but the compare stays case-insensitive so that a
         * caller may spell the name however it reads best. */
        if (header->name->len != name_len ||
            !h2o_lcstris(header->name->base, header->name->len, name, name_len))
            continue;
        if (len != NULL)
            *len = header->value.len;
        return header->value.base;
    }
    return NULL;
}

int sc_http_minor_version(const sc_http_req *req)
{
    const h2o_req_t *r = (const h2o_req_t *)req;

    if (r == NULL)
        return 0;
    /* h2o reports 0x101 for HTTP/1.1 and 0x200 for HTTP/2; anything at or above 1.1 answers 1. */
    return r->version >= 0x101 ? 1 : 0;
}

sc_status sc_http_header_add(sc_http_req *req, const char *name, const char *value,
                             size_t value_len)
{
    h2o_req_t *r = (h2o_req_t *)req;

    if (r == NULL || name == NULL || value == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (r->res.headers.size >= SC_HTTP_RESPONSE_HEADERS_MAX)
        return SC_ERR_TOO_LONG;
    /* h2o copies neither the name nor the value, so the value goes into the request pool --
     * which lives exactly as long as the request, and therefore longer than the handler that
     * asked for the header. The name is borrowed and must be a literal, which the header says. */
    h2o_add_header_by_str(&r->pool, &r->res.headers, name, strlen(name), 0, NULL,
                          h2o_strdup(&r->pool, value, value_len).base, value_len);
    return SC_OK;
}

/**
 * The status line, the Content-Type and the framing -- everything both reply functions do
 * before they differ over whether the body is copied.
 *
 * @return 1 when the answer is complete and nothing more is to be sent, which is what a status
 *         that carries no representation is.
 */
static int begin_reply(h2o_req_t *r, int status, const char *content_type, size_t body_len)
{
    r->res.status = status;
    /* The same table the other backend writes its status line from -- one status, one phrase,
     * whichever backend answered. It used to be "Error" for everything but 200 here, which was
     * invisible while the roles served nothing but 200s and 404s and became a difference a
     * client can see the moment a route answered 400. */
    r->res.reason = sc_http_reason(status);

    /* A 204 and a 304 carry no body, and neither may describe one. SIZE_MAX is how h2o is told
     * there is no content-length to send; it does not fall back to chunked for either status --
     * should_use_chunked_encoding refuses both explicitly -- so the answer is a status line, the
     * connection headers, whatever the handler added, and nothing else. The other backend
     * writes the same, which is what keeps the two indistinguishable to a client. */
    if (sc_http_status_omits_body(status)) {
        r->res.content_length = SIZE_MAX;
        if (r->_generator != NULL) {
            h2o_iovec_t empty = h2o_iovec_init(NULL, 0);
            h2o_send(r, &empty, 0, H2O_SEND_STATE_FINAL);
        } else {
            /* "" rather than NULL: h2o_send_inline copies through h2o_strdup, and a memcpy
             * from a null pointer is undefined even for zero bytes. */
            h2o_send_inline(r, "", 0);
        }
        return 1;
    }
    /* Set before sending, and this is not optional. h2o_send_inline deliberately leaves
     * content_length alone -- its own comment says so, because it also serves 304 responses --
     * and without it the HTTP/1 layer falls back to chunked. The other backend always sends a
     * Content-Length, so leaving this out is a difference a client can see. The integration
     * suite is what noticed; curl had been hiding it. */
    r->res.content_length = body_len;
    h2o_add_header(&r->pool, &r->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, content_type,
                   strlen(content_type));
    return 0;
}

sc_status sc_http_reply(sc_http_req *req, int status, const char *content_type, const char *body,
                        size_t body_len)
{
    h2o_req_t *r = (h2o_req_t *)req;

    if (r == NULL || content_type == NULL || body == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (begin_reply(r, status, content_type, body_len))
        return SC_OK;

    if (r->_generator != NULL) {
        /* A deferred request: sc_http_defer already started the response, and h2o_send_inline
         * asserts that nothing has. Send directly instead -- h2o clears the generator on a
         * final send, which is also what keeps `stop` from firing on a request that was
         * answered. The strdup is the same one h2o_send_inline does, and for the same reason:
         * the buffer has to outlive this call. */
        h2o_iovec_t buf = h2o_strdup(&r->pool, body, body_len);
        h2o_send(r, &buf, 1, H2O_SEND_STATE_FINAL);
        return SC_OK;
    }
    /* h2o_send_inline copies into the request pool itself, which is what makes it safe to hand
     * it the caller's stack buffer: the pool lives exactly as long as the request. */
    h2o_send_inline(r, body, body_len);
    return SC_OK;
}

sc_status sc_http_reply_static(sc_http_req *req, int status, const char *content_type,
                               const char *body, size_t body_len)
{
    h2o_req_t *r = (h2o_req_t *)req;
    /* h2o keeps no state in a generator that answers in one send, so one instance serves every
     * request on every loop -- which is what h2o_send_inline's own static generator is. */
    static h2o_generator_t generator = {NULL, NULL};
    h2o_iovec_t buf;

    if (r == NULL || content_type == NULL || body == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (begin_reply(r, status, content_type, body_len))
        return SC_OK;

    /* The point of this function: the iovec names the caller's bytes instead of a copy of them.
     * h2o holds the pointer until the write completes, and the header says what that costs the
     * caller -- bytes that outlive the process, which for an embedded file is free. */
    buf = h2o_iovec_init(body, body_len);
    /* A deferred request already has a generator, and h2o_start_response asserts that it does
     * not -- the same fork sc_http_reply has, for the same reason. */
    if (r->_generator == NULL)
        h2o_start_response(r, &generator);
    h2o_send(r, &buf, 1, H2O_SEND_STATE_FINAL);
    return SC_OK;
}
