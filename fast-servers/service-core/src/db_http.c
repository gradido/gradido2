/*
 * The executor's hooks, for an HTTP server. service_core/db_http.h holds the flow.
 */
#include "service_core/db_http.h"

static sc_status park(void *context, sc_db_unit *unit)
{
    sc_http_ticket ticket = 0;
    sc_status status =
        sc_http_defer((sc_http_server *)context, (sc_http_req *)unit->origin, unit, &ticket);

    if (status != SC_OK)
        return status == SC_ERR_QUEUE_FULL ? SC_ERR_QUEUE_FULL : status;
    unit->park_token = ticket;
    unit->origin = NULL; /* parked: from here on the request is reached through the ticket */
    return SC_OK;
}

static void finished(void *context, sc_db_unit *unit)
{
    /* On the worker. The loop does the rest, in resume(), below. */
    (void)sc_http_resume((sc_http_server *)context, (sc_http_ticket)unit->park_token);
}

static void resume(sc_http_req *req, void *work, void *user_data)
{
    sc_db_unit *unit = (sc_db_unit *)work;

    (void)user_data;
    if (unit != NULL && unit->done != NULL)
        unit->done(unit, req);
}

void sc_db_http_configure(sc_db_exec_config *cfg, sc_http_server *server)
{
    if (cfg == NULL)
        return;
    cfg->park = park;
    cfg->finished = finished;
    cfg->context = server;
}

sc_status sc_db_http_attach(sc_http_server *server)
{
    return sc_http_on_resume(server, resume, NULL);
}

sc_status sc_db_submit(sc_db_exec *exec, sc_http_req *req, sc_db_unit *unit)
{
    sc_status status;
    int ran = 0;

    if (exec == NULL || req == NULL || unit == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    unit->arena = sc_http_request_arena(req);
    unit->origin = req;
    status = sc_db_exec_submit(exec, sc_http_current_loop(), unit, &ran);
    if (status == SC_OK && ran && unit->done != NULL)
        unit->done(unit, req);
    return status;
}
