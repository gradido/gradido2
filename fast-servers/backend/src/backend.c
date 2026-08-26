#include "backend/backend.h"

#include "backend_core/backend_core.h"
#include "service_core/http.h"
#include "service_core/log.h"

sc_status backend_run(const sc_config *cfg, const sc_quit_flag *quit)
{
    sc_http_config http_config;
    sc_http_server *server;
    sc_status status;

    if (cfg == NULL || quit == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    status = backend_core_init(cfg);
    if (status != SC_OK)
        return status;

    http_config.host = cfg->listen_host;
    http_config.port = cfg->backend_port;
    http_config.role = "backend";
    http_config.threads = cfg->server_threads;

    server = sc_http_server_create(&http_config);
    if (server == NULL) {
        backend_core_shutdown();
        return SC_ERR_NO_MEMORY;
    }

    /* Registration, and only here. Everything contracted in contracts/server/backend/ joins
     * this list as it is implemented; the health route is operational and is not one of them. */
    status = sc_http_route(server, SC_HTTP_HEALTH_PATH, sc_http_health, (void *)"backend");
    if (status == SC_OK)
        status = sc_http_listen(server);
    if (status == SC_OK)
        status = sc_http_run(server, quit);
    else
        sc_log_fatal(SC_CAT_STARTUP, "server.start.failed", "backend did not start: %s",
                     sc_status_name(status));

    sc_http_server_destroy(server);
    backend_core_shutdown();
    return status;
}
