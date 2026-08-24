/*
 * The h2o backend: one event loop per server, the evloop socket layer, no libuv.
 *
 * The accept path follows h2o's own examples and the h2o prototype. What is different here
 * is that the loop is not `while (h2o_evloop_run(loop, INT32_MAX))`: it ticks, so the quit flag
 * a signal handler raised is seen within SC_RUNTIME_TICK_MS and every role shuts down together.
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

#include "h2o.h"
#include "h2o/http1.h"

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
} sc_route_handler;

struct sc_http_server {
    h2o_globalconf_t config;
    h2o_context_t context;
    h2o_accept_ctx_t accept_ctx;
    h2o_hostconf_t *hostconf;
    int listen_fd;
    char host[64];
    char role[32];
    uint16_t port;
    int context_started;
};

const char *sc_http_backend_name(void)
{
    return "h2o";
}

static int on_request(h2o_handler_t *self, h2o_req_t *req)
{
    sc_route_handler *route = (sc_route_handler *)self;
    return route->fn((sc_http_req *)req, route->user_data);
}

sc_http_server *sc_http_server_create(const sc_http_config *cfg)
{
    sc_http_server *server;

    if (cfg == NULL || cfg->host == NULL || cfg->role == NULL)
        return NULL;
    if (strlen(cfg->host) >= sizeof(server->host) || strlen(cfg->role) >= sizeof(server->role))
        return NULL;

    server = (sc_http_server *)calloc(1, sizeof(*server));
    if (server == NULL)
        return NULL;
    server->listen_fd = -1;
    server->port = cfg->port;
    memcpy(server->host, cfg->host, strlen(cfg->host) + 1);
    memcpy(server->role, cfg->role, strlen(cfg->role) + 1);

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
    if (server == NULL)
        return;
    if (server->context_started)
        h2o_context_dispose(&server->context);
    if (server->listen_fd != -1)
        (void)close(server->listen_fd);
    h2o_config_dispose(&server->config);
    free(server);
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
    return SC_OK;
}

static void on_accept(h2o_socket_t *listener, const char *err)
{
    sc_http_server *server = (sc_http_server *)listener->data;
    h2o_socket_t *sock;

    if (err != NULL)
        return;
    if ((sock = h2o_evloop_socket_accept(listener)) == NULL)
        return;
    h2o_accept(&server->accept_ctx, sock);
}

sc_status sc_http_listen(sc_http_server *server)
{
    struct sockaddr_in addr;
    h2o_socket_t *sock;
    int reuse = 1;

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

    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd == -1 ||
        setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(server->listen_fd, SOMAXCONN) != 0) {
        sc_log_error(SC_CAT_STARTUP, "server.listen.failed", "%s cannot listen on %s:%u: %s",
                     server->role, server->host, (unsigned)server->port, strerror(errno));
        return SC_ERR_NETWORK;
    }

    /* The context owns the loop; both are created here rather than in _create so that a server
     * that never got its port does not leave an event loop behind. */
    h2o_context_init(&server->context, h2o_evloop_create(), &server->config);
    server->context_started = 1;
    server->accept_ctx.ctx = &server->context;
    server->accept_ctx.hosts = server->config.hosts;

    sock = h2o_evloop_socket_create(server->context.loop, server->listen_fd,
                                    H2O_SOCKET_FLAG_DONT_READ);
    if (sock == NULL)
        return SC_ERR_NETWORK;
    sock->data = server;
    h2o_socket_read_start(sock, on_accept);

    sc_log_info(SC_CAT_STARTUP, "server.listen", "%s listening on http://%s:%u", server->role,
                server->host, (unsigned)server->port);
    return SC_OK;
}

sc_status sc_http_run(sc_http_server *server, const sc_quit_flag *quit)
{
    if (server == NULL || quit == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    while (!sc_quit_requested(quit)) {
        if (h2o_evloop_run(server->context.loop, SC_RUNTIME_TICK_MS) != 0)
            return SC_ERR_NETWORK;
    }
    sc_log_info(SC_CAT_STARTUP, "server.stop", "%s stopped", server->role);
    return SC_OK;
}

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

sc_status sc_http_reply(sc_http_req *req, int status, const char *content_type, const char *body,
                        size_t body_len)
{
    h2o_req_t *r = (h2o_req_t *)req;

    if (r == NULL || content_type == NULL || body == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    r->res.status = status;
    r->res.reason = status == 200 ? "OK" : "Error";
    /* Set before sending, and this is not optional. h2o_send_inline deliberately leaves
     * content_length alone -- its own comment says so, because it also serves 304 responses --
     * and without it the HTTP/1 layer falls back to chunked. The other backend always sends a
     * Content-Length, so leaving this out is a difference a client can see. The integration
     * suite is what noticed; curl had been hiding it. */
    r->res.content_length = body_len;
    h2o_add_header(&r->pool, &r->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, content_type,
                   strlen(content_type));
    /* h2o_send_inline copies into the request pool itself, which is what makes it safe to hand
     * it the caller's stack buffer: the pool lives exactly as long as the request. */
    h2o_send_inline(r, body, body_len);
    return SC_OK;
}
