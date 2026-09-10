/*
 * Everything that has to be true before a request can be served, in the order it becomes true.
 *
 * The counterpart of packages/backend's `open()`: the database answers, its schema is current,
 * and this instance knows which community it is. Nothing here asks anybody anything -- an empty
 * database ends the start with a line naming the `setup` command.
 *
 * All the failures have one outcome, so they are reported as one line each and the caller only
 * has to decide whether to go on: a database that will not come, will not migrate or has no
 * community ends the role here, where the reason is still visible, instead of turning every
 * request into a 500.
 */
#include "backend_core/backend_core.h"

#include <string.h>

#include "backend_core/database/migrations.h"
#include "service_core/db_http.h"
#include "service_core/log/log.h"

/*
 * The two startup reads, on one connection opened for them and closed afterwards: the database
 * may still be starting, so this is the connection that waits for it, and everything after it
 * can then expect the database to answer at once.
 */
static sc_status migrate_and_find_home(const sc_db_config *db_config, const sc_quit_flag *quit,
                                       bc_context *context, sc_log_context *log, int *found)
{
    char error[BC_SQL_ERROR_MAX];
    sc_db *db = NULL;
    sc_status status;

    status = sc_db_open_waiting(db_config, quit, &db);
    if (status != SC_OK) {
        /* The driver's own sentence is already on the db.connection.failed lines above this one;
         * this is the line that says the run is over. */
        sc_log_event(SC_LOG_FATAL, SC_CAT_STARTUP, "startup.database.failed", log,
                     "cannot reach the database");
        return status;
    }

    status = bc_migrations_run(db, NULL);
    if (status != SC_OK) {
        /* A schema this build cannot run against was already reported as db.migration.denied,
         * with the migration named and what to do about it. Saying it again under a heading
         * about reaching the database would only make the useful line harder to find. */
        if (status != SC_ERR_MALFORMED)
            sc_log_event(SC_LOG_FATAL, SC_CAT_STARTUP, "startup.database.failed", log,
                         "the database could not be migrated");
        sc_db_close(db);
        return status;
    }

    status = bc_community_find_home(db, &context->home, found, error, sizeof(error));
    sc_db_close(db);
    if (status != SC_OK)
        sc_log_event(SC_LOG_FATAL, SC_CAT_STARTUP, "startup.database.failed", log,
                     "cannot read the home community: %s", error);
    return status;
}

sc_status bc_context_open(const sc_db_config *db_config, sc_http_server *server,
                          const sc_topology *topology, const sc_quit_flag *quit, bc_context *out)
{
    sc_log_value db_field[1];
    sc_log_context log = {0};
    sc_db_exec_config exec_config;
    sc_db_exec_stats stats;
    uint16_t opened = 0;
    uint16_t wanted;
    int found = 0;
    sc_status status;

    if (db_config == NULL || server == NULL || out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    db_field[0] = (sc_log_value)SC_LOG_STR("db", sc_db_kind_name(db_config->kind));
    log.data = db_field;
    log.data_count = 1;

    status = migrate_and_find_home(db_config, quit, out, &log, &found);
    if (status != SC_OK)
        return status;
    if (!found) {
        /*
         * There is no second case here and that is the point. There is no third one either,
         * because `users.community_id` is NOT NULL: without this row nothing can register, so
         * serving without it would only mean failing later and less clearly.
         */
        sc_log_value reason[1] = {SC_LOG_STR("reason", "not-set-up")};
        sc_log_context setup_log = {0};

        setup_log.data = reason;
        setup_log.data_count = 1;
        sc_log_event(SC_LOG_FATAL, SC_CAT_STARTUP, "startup.setup.failed", &setup_log,
                     "cannot start: this database has no community yet. Run the setup command "
                     "once, with a terminal attached, to say who this community is -- under "
                     "docker compose that is: docker compose run --rm backend setup");
        return SC_ERR_UNAVAILABLE;
    }

    memset(&exec_config, 0, sizeof(exec_config));
    exec_config.db = db_config;
    exec_config.loops = sc_http_server_threads(server);
    exec_config.topology = topology;
    sc_db_http_configure(&exec_config, server);
    wanted = sc_db_exec_workers_for(&exec_config);
    if (db_config->kind == SC_DB_SQLITE)
        wanted = (uint16_t)(wanted + exec_config.loops);

    status = sc_db_exec_open(&exec_config, &out->exec, &opened);
    if (status != SC_OK) {
        /* The database answered a moment ago, so a refusal now is the size of the pool rather
         * than the database being away -- and the driver's reason is on the line above. */
        sc_log_event(SC_LOG_FATAL, SC_CAT_STARTUP, "startup.database.failed", &log,
                     "the database took %u of the %u connections this process holds -- its "
                     "limits (max_connections, and any on the role or the database) have to "
                     "cover DB_POOL_SIZE for every serving process, plus room to administer it",
                     (unsigned)opened, (unsigned)wanted);
        return status;
    }
    status = sc_db_http_attach(server);
    if (status != SC_OK) {
        bc_context_close(out);
        return status;
    }

    sc_db_exec_stats_of(out->exec, &stats);
    sc_log_info(SC_CAT_STARTUP, "config.database",
                "%u database worker%s in %u cache group%s, serving %u loop%s",
                (unsigned)stats.workers, stats.workers == 1 ? "" : "s", (unsigned)stats.groups,
                stats.groups == 1 ? "" : "s", (unsigned)exec_config.loops,
                exec_config.loops == 1 ? "" : "s");
    return SC_OK;
}

void bc_context_close(bc_context *context)
{
    if (context == NULL)
        return;
    /* After the loops have stopped submitting and before the server is destroyed: the workers
     * drain what is queued and hand each unit back through the server, which must still exist
     * for that. */
    sc_db_exec_close(context->exec);
    context->exec = NULL;
}
