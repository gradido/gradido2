/*
 * `GET /peer/bootstrap` -- contracts/server/backend/peer.json. The answer comes from the dht-node
 * role when it runs in this process; packages/backend/src/server/peerRoutes.ts on the reference
 * path.
 */
#include "routes.h"

#include <stdlib.h>

#include "service_core/api_error.h"
#include "service_core/log/log.h"
#include "service_core/peer_network.h"

/* DHT_BOOTSTRAP_PEERS_MAX in contracts/const.json. */
#define BACKEND_BOOTSTRAP_PEERS_MAX 20u
/* Twenty peers with a handful of addresses each, and the node itself. */
#define BACKEND_BOOTSTRAP_BODY_BYTES (64u * 1024u)
#define ROUTE_PATH "/peer/bootstrap"

/* http.request.failed as contracts/logging.json fixes it; the client is told UNKNOWN only. */
static int reply_unknown(sc_http_req *req, const char *why)
{
    sc_log_value data[3] = {SC_LOG_STR("method", "GET"), SC_LOG_STR("path", ROUTE_PATH),
                            SC_LOG_UINT("status", (uint64_t)sc_api_error_status(SC_API_UNKNOWN))};
    sc_log_context log = {0};

    log.err_name = sc_api_error_name(SC_API_UNKNOWN);
    log.err_code = SC_API_UNKNOWN;
    log.data = data;
    log.data_count = 3;
    sc_log_event(SC_LOG_ERROR, SC_CAT_HTTP, "http.request.failed", &log,
                 "unhandled error while serving %s: %s", ROUTE_PATH, why);
    (void)sc_http_reply_unknown(req);
    return 0;
}

int backend_peer_bootstrap(sc_http_req *req, void *user_data)
{
    char *body;
    size_t len = 0;
    sc_status status;

    if (!sc_http_method_is(req, "GET"))
        return backend_route_not_implemented(req, user_data);

    body = (char *)malloc(BACKEND_BOOTSTRAP_BODY_BYTES);
    if (body == NULL)
        return reply_unknown(req, "no memory for the answer");
    status = sc_peer_network_bootstrap_answer(BACKEND_BOOTSTRAP_PEERS_MAX, body,
                                              BACKEND_BOOTSTRAP_BODY_BYTES, &len);
    if (status == SC_OK) {
        (void)sc_http_reply(req, 200, "application/json", body, len);
    } else if (status == SC_ERR_UNAVAILABLE) {
        (void)sc_http_reply_peer_network_unavailable(req);
    } else {
        (void)reply_unknown(req, sc_status_name(status));
    }
    free(body);
    return 0;
}
