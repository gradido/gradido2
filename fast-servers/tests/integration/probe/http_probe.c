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

#include "service_core/http.h"
#include "service_core/log.h"
#include "service_core/runtime.h"

static sc_quit_flag g_quit;

/** Fixed answer, so the suite has something whose bytes it knows exactly. */
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
    char *end;

    if (argc != 2) {
        fprintf(stderr, "usage: http-probe <port>\n");
        return 2;
    }
    port = strtoul(argv[1], &end, 10);
    if (*end != '\0' || port == 0 || port > 65535) {
        fprintf(stderr, "http-probe: '%s' is not a port\n", argv[1]);
        return 2;
    }

    sc_log_init(SC_LOG_WARN);
    sc_runtime_install_signal_handlers(&g_quit);

    http_config.host = "127.0.0.1";
    http_config.port = (uint16_t)port;
    http_config.role = "http-probe";
    server = sc_http_server_create(&http_config);
    if (server == NULL)
        return 1;

    if (sc_http_route(server, "/hello", handle_hello, NULL) != SC_OK ||
        sc_http_route(server, "/echo", handle_echo, NULL) != SC_OK ||
        sc_http_route(server, "/echo-header", handle_echo_header, NULL) != SC_OK ||
        sc_http_route(server, "/whoami", handle_whoami, NULL) != SC_OK ||
        sc_http_route(server, "/path", handle_path, NULL) != SC_OK ||
        sc_http_listen(server) != SC_OK) {
        sc_http_server_destroy(server);
        return 1;
    }

    /* Announced on stdout rather than through the log, so the harness has one line to wait for
     * that does not depend on the log level. */
    printf("http-probe listening on 127.0.0.1:%lu backend=%s\n", port, sc_http_backend_name());
    fflush(stdout);

    (void)sc_http_run(server, &g_quit);
    sc_http_server_destroy(server);
    return 0;
}
