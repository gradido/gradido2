#include "federation/federation.h"

#include "backend_core/backend_core.h"
#include "service_core/http.h"
#include "service_core/log.h"

sc_status federation_run(const sc_config *cfg, const sc_quit_flag *quit)
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
    http_config.port = cfg->federation_port;
    http_config.role = "federation";
    http_config.threads = cfg->server_threads;

    server = sc_http_server_create(&http_config);
    if (server == NULL) {
        backend_core_shutdown();
        return SC_ERR_NO_MEMORY;
    }

    status = sc_http_route(server, SC_HTTP_HEALTH_PATH, sc_http_health, (void *)"federation");
    if (status == SC_OK)
        status = sc_http_listen(server);
    if (status == SC_OK)
        status = sc_http_run(server, quit);
    else
        sc_log_fatal(SC_CAT_STARTUP, "server.start.failed", "federation did not start: %s",
                     sc_status_name(status));

    sc_http_server_destroy(server);
    backend_core_shutdown();
    return status;
}
