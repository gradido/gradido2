/*
 * The server the integration suite drives. Not a role, and not shipped.
 *
 * The suite exists to check that h2o and the fallback do the same thing to the same bytes, and
 * that needs routes that report what arrived: a body echoed back, a header value returned, the
 * method and version as parsed. `/_health` cannot show any of that -- it answers the same
 * thirty bytes whatever it is sent.
 *
 * Those routes do not belong in backend or federation. A route that exists so a test can see
 * something is a route an operator can reach, and "no feature originates in the fast path"
 * covers test conveniences first of all. So this is a second consumer of service_core/http.h,
 * built only under -Dtests, and the production roles keep exactly the one route they have.
 *
 * That it compiles at all is part of what is being tested: the seam is meant to carry a server
 * that is not one of the three roles.
 *
 *   http-probe <port>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "service_core/http.h"
#include "service_core/log.h"
#include "service_core/runtime.h"

static sc_quit_flag g_quit;

/*
 * The worker behind /defer, and the whole reason this route exists: sc_http_defer and
 * sc_http_resume cannot be tested from the loop that owns the request, because the thing being
 * tested is that the answer comes back to that loop from somewhere else.
 *
 * It is the shape a real one has -- a fixed pool taken under a lock, a condition variable, a
 * worker that sends the ticket back when its work is done -- in as few lines as still make the
 * point. Nothing allocates after startup here either.
 */
#define WORK_MAX 64

typedef struct work_item {
    sc_http_ticket ticket;
    unsigned delay_ms;
    char echo[128];
    size_t echo_len;
    int used;
} work_item;

static struct {
    uv_mutex_t lock;
    uv_cond_t wake;
    uv_thread_t thread;
    work_item items[WORK_MAX];
    int32_t queue[WORK_MAX];
    int head, tail, count;
    int stop;
    /* Deferred requests whose client had gone by the time the work finished. The suite reads
     * it back from /defer-stats, because a resume that arrives at nobody is otherwise
     * indistinguishable from one that never arrived. */
    unsigned abandoned;
    unsigned answered;
    sc_http_server *server;
} g_worker;

static work_item *take_item(void)
{
    int i;

    uv_mutex_lock(&g_worker.lock);
    for (i = 0; i != WORK_MAX; ++i) {
        if (!g_worker.items[i].used) {
            g_worker.items[i].used = 1;
            uv_mutex_unlock(&g_worker.lock);
            return &g_worker.items[i];
        }
    }
    uv_mutex_unlock(&g_worker.lock);
    return NULL;
}

static void give_back(work_item *item)
{
    uv_mutex_lock(&g_worker.lock);
    item->used = 0;
    uv_mutex_unlock(&g_worker.lock);
}

static void push(work_item *item)
{
    uv_mutex_lock(&g_worker.lock);
    g_worker.queue[g_worker.tail] = (int32_t)(item - g_worker.items);
    g_worker.tail = (g_worker.tail + 1) % WORK_MAX;
    ++g_worker.count;
    uv_cond_signal(&g_worker.wake);
    uv_mutex_unlock(&g_worker.lock);
}

static void run_worker(void *arg)
{
    (void)arg;
    for (;;) {
        work_item *item;

        uv_mutex_lock(&g_worker.lock);
        while (g_worker.count == 0 && !g_worker.stop)
            uv_cond_wait(&g_worker.wake, &g_worker.lock);
        if (g_worker.stop && g_worker.count == 0) {
            uv_mutex_unlock(&g_worker.lock);
            return;
        }
        item = &g_worker.items[g_worker.queue[g_worker.head]];
        g_worker.head = (g_worker.head + 1) % WORK_MAX;
        --g_worker.count;
        uv_mutex_unlock(&g_worker.lock);

        /* Standing in for a commit: time spent off the loop, on this thread. */
        if (item->delay_ms != 0)
            sc_runtime_sleep_ms(item->delay_ms);
        (void)sc_http_resume(g_worker.server, item->ticket);
    }
}

/** Runs on the loop that owns the request, and nowhere else. */
static void on_resume(sc_http_req *req, void *work, void *user_data)
{
    work_item *item = (work_item *)work;
    (void)user_data;

    uv_mutex_lock(&g_worker.lock);
    if (req != NULL)
        ++g_worker.answered;
    else
        ++g_worker.abandoned;
    uv_mutex_unlock(&g_worker.lock);

    /* NULL is a client that hung up while the worker was busy. The work still happened; there
     * is just nobody left to tell, and the item has to go back either way. */
    if (req != NULL)
        (void)sc_http_reply(req, 200, "text/plain", item->echo, item->echo_len);
    give_back(item);
}

/**
 * Answers later, from another thread. `X-Defer-Ms` says how much later; the body comes back as
 * the answer, so the suite can see that the request survived the wait intact.
 */
static int handle_defer(sc_http_req *req, void *user_data)
{
    sc_http_server *server = (sc_http_server *)user_data;
    size_t body_len = 0, header_len = 0;
    const char *body = sc_http_body(req, &body_len);
    const char *delay = sc_http_header(req, "x-defer-ms", &header_len);
    work_item *item;
    sc_status status;

    item = take_item();
    if (item == NULL) {
        (void)sc_http_reply(req, 503, "text/plain", "no worker slot\n", 15);
        return 0;
    }
    item->delay_ms = 0;
    if (delay != NULL && header_len != 0 && header_len < 8) {
        char number[8];
        memcpy(number, delay, header_len);
        number[header_len] = '\0';
        item->delay_ms = (unsigned)strtoul(number, NULL, 10);
    }
    item->echo_len = body_len < sizeof(item->echo) ? body_len : sizeof(item->echo);
    if (item->echo_len != 0)
        memcpy(item->echo, body, item->echo_len);

    /* Deferring first and queueing second is not a preference: the worker needs the ticket, and
     * a ticket that reached it before the request was deferred would name nothing. */
    status = sc_http_defer(server, req, item, &item->ticket);
    if (status != SC_OK) {
        give_back(item);
        /* A full table answers rather than waits -- Architecture.md, *The write must be
         * answered, not acknowledged*. */
        (void)sc_http_reply(req, 503, "text/plain", "deferral refused\n", 17);
        return 0;
    }
    push(item);
    return 0;
}

/** What the deferred requests did, so the suite can see the ones nobody was left to answer. */
static int handle_defer_stats(sc_http_req *req, void *user_data)
{
    char out[128];
    int written;
    unsigned answered, abandoned;
    (void)user_data;

    uv_mutex_lock(&g_worker.lock);
    answered = g_worker.answered;
    abandoned = g_worker.abandoned;
    uv_mutex_unlock(&g_worker.lock);

    written = snprintf(out, sizeof(out), "{\"answered\":%u,\"abandoned\":%u,\"threads\":%u}",
                       answered, abandoned, (unsigned)sc_http_thread_count(g_worker.server));
    if (written <= 0 || (size_t)written >= sizeof(out)) {
        (void)sc_http_reply(req, 500, "text/plain", "", 0);
        return 0;
    }
    (void)sc_http_reply(req, 200, "application/json", out, (size_t)written);
    return 0;
}

/** Fixed answer, so the suite has something whose bytes it knows exactly. */
/*
 * A body that is bigger than any buffer either backend serialises a response into, answered
 * with sc_http_reply_static.
 *
 * That function exists for the static web server -- an embedded page is bytes that outlive the
 * process, so they are handed to the socket rather than copied -- and the size is the point:
 * the fallback backend puts a response head in a 16 KiB buffer and copies an ordinary body into
 * an 8 KiB one, so a 100 KB file only goes out at all because the body is written beside the
 * head instead of into it. h2o has no such limit and is here for the other half of the claim:
 * that both backends put the same bytes on the wire.
 *
 * Static storage, filled once before the server starts, which is what the "outlives the
 * process" contract asks for.
 */
#define STATIC_BODY_LEN (100 * 1024)

static char g_static_body[STATIC_BODY_LEN];

static void fill_static_body(void)
{
    size_t i;

    /* Every byte value in order, so a test can check the whole thing arrived in order rather
     * than merely arrived at the right length. */
    for (i = 0; i != STATIC_BODY_LEN; ++i)
        g_static_body[i] = (char)(i % 256);
}

static int handle_static(sc_http_req *req, void *user_data)
{
    (void)user_data;

    if (!sc_http_method_is(req, "GET"))
        return -1;
    (void)sc_http_header_add(req, "etag", "\"probe\"", sizeof("\"probe\"") - 1);
    (void)sc_http_reply_static(req, 200, "application/octet-stream", g_static_body,
                               STATIC_BODY_LEN);
    return 0;
}

/**
 * A 304, which is what the static server answers a revalidation with.
 *
 * Neither backend may describe a body on one -- no Content-Length, no Content-Type -- and both
 * have to keep the headers the handler added, because those are what the cache came for. The
 * connection has to stay usable afterwards, which is the part that goes wrong when a response
 * says nothing about its framing.
 */
static int handle_not_modified(sc_http_req *req, void *user_data)
{
    (void)user_data;

    if (!sc_http_method_is(req, "GET"))
        return -1;
    (void)sc_http_header_add(req, "etag", "\"probe\"", sizeof("\"probe\"") - 1);
    (void)sc_http_reply(req, 304, "text/plain", "ignored", 7);
    return 0;
}

static int handle_hello(sc_http_req *req, void *user_data)
{
    static const char kBody[] = "Hello World\n";
    (void)user_data;

    if (!sc_http_method_is(req, "GET"))
        return -1;
    (void)sc_http_reply(req, 200, "text/plain", kBody, sizeof(kBody) - 1);
    return 0;
}

/**
 * Echoes the request body. This is the route that makes framing visible: whether the body was
 * collected in full before the handler ran, whether a chunked body was reassembled in order,
 * and whether a pipelined second request was parsed off the first one's body.
 */
static int handle_echo(sc_http_req *req, void *user_data)
{
    size_t len = 0;
    const char *body = sc_http_body(req, &len);
    (void)user_data;

    (void)sc_http_reply(req, 200, "application/octet-stream", body != NULL ? body : "", len);
    return 0;
}

/** Returns the X-Test header, so the suite can see a value with colons and spaces come back
 *  whole and a lookup succeed on a differently-cased name. */
static int handle_echo_header(sc_http_req *req, void *user_data)
{
    size_t len = 0;
    const char *value = sc_http_header(req, "x-test", &len);
    (void)user_data;

    if (value == NULL) {
        (void)sc_http_reply(req, 400, "text/plain", "no X-Test header\n", 17);
        return 0;
    }
    (void)sc_http_reply(req, 200, "text/plain", value, len);
    return 0;
}

/** Reports what was parsed rather than leaving the suite to infer it. */
static int handle_whoami(sc_http_req *req, void *user_data)
{
    char out[128];
    size_t body_len = 0;
    int written;
    (void)user_data;

    (void)sc_http_body(req, &body_len);
    written = snprintf(out, sizeof(out), "%s HTTP/1.%d body=%zu\n",
                       sc_http_method_is(req, "PUT")    ? "PUT"
                       : sc_http_method_is(req, "POST") ? "POST"
                       : sc_http_method_is(req, "GET")  ? "GET"
                                                        : "OTHER",
                       sc_http_minor_version(req), body_len);
    if (written <= 0 || (size_t)written >= sizeof(out)) {
        (void)sc_http_reply(req, 500, "text/plain", "", 0);
        return 0;
    }
    (void)sc_http_reply(req, 200, "text/plain", out, (size_t)written);
    return 0;
}

/** Returns the path the router matched, so a query string can be seen not to reach it. */
static int handle_path(sc_http_req *req, void *user_data)
{
    size_t len = 0;
    const char *path = sc_http_path(req, &len);
    (void)user_data;

    (void)sc_http_reply(req, 200, "text/plain", path != NULL ? path : "", len);
    return 0;
}

int main(int argc, char **argv)
{
    sc_http_config http_config;
    sc_http_server *server;
    unsigned long port;
    unsigned long threads = 0;
    char *end;

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: http-probe <port> [threads]\n");
        return 2;
    }
    port = strtoul(argv[1], &end, 10);
    if (*end != '\0' || port == 0 || port > 65535) {
        fprintf(stderr, "http-probe: '%s' is not a port\n", argv[1]);
        return 2;
    }
    /* Absent means what it means for a role: one loop per core. The suite passes a number where
     * it wants the answer not to depend on the machine it runs on. */
    if (argc == 3) {
        threads = strtoul(argv[2], &end, 10);
        if (*end != '\0' || threads > SC_HTTP_THREADS_MAX) {
            fprintf(stderr, "http-probe: '%s' is not a thread count\n", argv[2]);
            return 2;
        }
    }

    sc_log_init(SC_LOG_WARN);
    sc_runtime_install_signal_handlers(&g_quit);

    http_config.host = "127.0.0.1";
    http_config.port = (uint16_t)port;
    http_config.role = "http-probe";
    http_config.threads = (uint16_t)threads;
    server = sc_http_server_create(&http_config);
    if (server == NULL)
        return 1;

    g_worker.server = server;
    if (uv_mutex_init(&g_worker.lock) != 0 || uv_cond_init(&g_worker.wake) != 0) {
        sc_http_server_destroy(server);
        return 1;
    }

    fill_static_body();

    if (sc_http_route(server, "/hello", handle_hello, NULL) != SC_OK ||
        sc_http_route(server, "/static", handle_static, NULL) != SC_OK ||
        sc_http_route(server, "/not-modified", handle_not_modified, NULL) != SC_OK ||
        sc_http_route(server, "/echo", handle_echo, NULL) != SC_OK ||
        sc_http_route(server, "/echo-header", handle_echo_header, NULL) != SC_OK ||
        sc_http_route(server, "/whoami", handle_whoami, NULL) != SC_OK ||
        sc_http_route(server, "/path", handle_path, NULL) != SC_OK ||
        sc_http_route(server, "/defer", handle_defer, server) != SC_OK ||
        sc_http_route(server, "/defer-stats", handle_defer_stats, NULL) != SC_OK ||
        sc_http_on_resume(server, on_resume, NULL) != SC_OK || sc_http_listen(server) != SC_OK) {
        sc_http_server_destroy(server);
        return 1;
    }
    if (uv_thread_create(&g_worker.thread, run_worker, NULL) != 0) {
        sc_http_server_destroy(server);
        return 1;
    }

    /* Announced on stdout rather than through the log, so the harness has one line to wait for
     * that does not depend on the log level. */
    printf("http-probe listening on 127.0.0.1:%lu backend=%s threads=%u\n", port,
           sc_http_backend_name(), (unsigned)sc_http_thread_count(server));
    fflush(stdout);

    (void)sc_http_run(server, &g_quit);

    uv_mutex_lock(&g_worker.lock);
    g_worker.stop = 1;
    uv_cond_signal(&g_worker.wake);
    uv_mutex_unlock(&g_worker.lock);
    (void)uv_thread_join(&g_worker.thread);

    sc_http_server_destroy(server);
    return 0;
}
