#include "backend/backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_core/backend_core.h"
#include "backend_core/database/migrations.h"
#include "cors.h"
#include "routes.h"
#include "service_core/db.h"
#include "service_core/http.h"
#include "service_core/log.h"
#include "setup.h"

sc_status backend_run(const sc_config *cfg, const sc_quit_flag *quit)
{
    sc_db_config db_config;
    bc_context context;
    sc_http_config http_config;
    sc_http_server *server;
    backend_cors_policy cors;
    const char *node_env;
    sc_status status;

    if (cfg == NULL || quit == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    /* Read here rather than carried on sc_config, the way db.c reads it: NODE_ENV says what kind
     * of installation this is, which two unrelated decisions happen to need, and neither of them
     * is a knob an admin turns. */
    node_env = getenv("NODE_ENV");
    cors.development = node_env == NULL || strcmp(node_env, "development") == 0;

    status = backend_core_init(cfg);
    if (status != SC_OK)
        return status;

    status = sc_db_config_load(&db_config);
    if (status != SC_OK) {
        backend_core_shutdown();
        return status;
    }
    sc_db_config_log(&db_config);

    /* Everything that has to be true before a request can be served: the database answers, its
     * schema is current, and this instance knows which community it is. Each failure is already
     * a line of its own by the time this returns -- see bc_context_open -- so the role only has
     * to stop. */
    status = bc_context_open(&db_config, quit, backend_ask_for_home_community, &context);
    if (status != SC_OK) {
        backend_core_shutdown();
        return status;
    }

    http_config.host = cfg->listen_host;
    http_config.port = cfg->backend_port;
    http_config.role = "backend";
    http_config.threads = cfg->server_threads;

    server = sc_http_server_create(&http_config);
    if (server == NULL) {
        bc_context_close(&context);
        backend_core_shutdown();
        return SC_ERR_NO_MEMORY;
    }

    /* Registration, and only here. Everything contracted in contracts/server/backend/ joins this
     * list as it is implemented; the health route is operational and is not one of them. */
    status = sc_http_route(server, SC_HTTP_HEALTH_PATH, sc_http_health, (void *)"backend");
    if (status == SC_OK)
        status = sc_http_route(server, "/user/create", backend_user_create, &context);
    /* Everything else is a contracted route this implementation does not serve yet, and it says
     * so rather than 404ing: a deployment runs one implementation and never forwards to the
     * other, so "not here" has to be distinguishable from "nowhere". */
    if (status == SC_OK)
        status = sc_http_route_default(server, backend_route_not_implemented, NULL);
    /* Before all of them, because which origins may call this server is a property of the
     * deployment and not of a path -- and because a rule that has to be remembered at every new
     * route is a rule that will be forgotten at one. */
    if (status == SC_OK)
        status = sc_http_before_route(server, backend_cors, &cors);
    if (status == SC_OK)
        status = sc_http_listen(server);

    if (status == SC_OK) {
        sc_log_value data[3] = {SC_LOG_STR("impl", "fast"),
                                SC_LOG_STR("db", sc_db_kind_name(db_config.kind)),
                                SC_LOG_UINT("port", cfg->backend_port)};

        sc_log_info(SC_CAT_STARTUP, "server.cors", "cross-origin: %s",
                    cors.development ? "any origin, which is what development is for"
                                     : "loopback only");
        sc_log_context log = {0};

        log.data = data;
        log.data_count = 3;
        sc_log_event(SC_LOG_INFO, SC_CAT_STARTUP, "startup.server.started", &log,
                     "backend listening on http://%s:%u", cfg->listen_host,
                     (unsigned)cfg->backend_port);
        status = sc_http_run(server, quit);
    } else {
        sc_log_fatal(SC_CAT_STARTUP, "server.start.failed", "backend did not start: %s",
                     sc_status_name(status));
    }

    sc_http_server_destroy(server);
    bc_context_close(&context);
    backend_core_shutdown();
    return status;
}

/** What DB_MIGRATE_DOWN is set to when the step to undo is the first one. */
#define EMPTY_DATABASE BC_MIGRATE_DOWN_EMPTY_TARGET

sc_status backend_migrate_down(const sc_config *cfg, const sc_quit_flag *quit)
{
    sc_db_config db_config;
    sc_db *db = NULL;
    char error[BC_SQL_ERROR_MAX];
    const char *node_env = getenv("NODE_ENV");
    const char *confirmation = getenv("DB_MIGRATE_DOWN");
    int release;
    sc_status status;

    if (cfg == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    release = node_env != NULL && strcmp(node_env, "production") == 0;
    if (confirmation != NULL && confirmation[0] == '\0')
        confirmation = NULL;

    status = sc_db_config_load(&db_config);
    if (status != SC_OK)
        return status;
    sc_db_config_log(&db_config);

    if (!release) {
        /* Said before it happens, so a developer meets the variable long before the day they
         * need it on a release. Not a contracted event: it reports how this run was invoked,
         * nothing about the database. */
        sc_log_warn(SC_CAT_DB, "db.migration.unconfirmed",
                    "migrating down without confirmation, which only development allows; a "
                    "release needs DB_MIGRATE_DOWN set to the migration one lower");
    }

    status = sc_db_open_waiting(&db_config, quit, &db);
    if (status != SC_OK) {
        sc_log_value data[1] = {SC_LOG_STR("db", sc_db_kind_name(db_config.kind))};
        sc_log_context log = {0};

        log.data = data;
        log.data_count = 1;
        sc_log_event(SC_LOG_FATAL, SC_CAT_STARTUP, "startup.database.failed", &log,
                     "cannot reach the database");
        return status;
    }

    if (release && confirmation == NULL) {
        (void)snprintf(error, sizeof(error),
                       "refusing to migrate down on a release without confirmation. Set "
                       "DB_MIGRATE_DOWN to the migration the database should end at -- one lower "
                       "than where it is, or %s for an empty database. This destroys whatever the "
                       "migration being undone held; read its down file in contracts/migrations "
                       "first.",
                       EMPTY_DATABASE);
        status = SC_ERR_INVALID_ARGUMENT;
    } else {
        /* Handed in rather than compared afterwards: which migration is at the head is a
         * property of the database, so only bc_migrations_down can hold the confirmation against
         * it before doing anything. A comparison after the fact reports what already happened. */
        status = bc_migrations_down(db, release ? confirmation : NULL, error, sizeof(error));
    }

    if (status != SC_OK) {
        /* Not db.migration.denied -- that one is about a schema this build cannot run against at
         * all, and carries the migration it diverges at. This is the down command declining, and
         * the reason is in the sentence rather than in a field, the way db.connection.failed
         * carries the driver's own message. */
        sc_log_value data[1] = {SC_LOG_STR("db", sc_db_kind_name(db_config.kind))};
        sc_log_context log = {0};

        log.data = data;
        log.data_count = 1;
        sc_log_event(SC_LOG_FATAL, SC_CAT_DB, "db.migration.refused", &log, "%s", error);
    }
    sc_db_close(db);
    return status;
}
