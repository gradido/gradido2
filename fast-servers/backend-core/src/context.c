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
#include "service_core/log/log.h"

sc_status bc_context_open(const sc_db_config *db_config, const sc_quit_flag *quit,
                          bc_context *out)
{
    char error[BC_SQL_ERROR_MAX];
    sc_log_value db_field[1];
    sc_log_context log = {0};
    int found = 0;
    sc_status status;

    if (db_config == NULL || out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (uv_mutex_init(&out->db_lock) != 0)
        return SC_ERR_NO_MEMORY;
    db_field[0] = (sc_log_value)SC_LOG_STR("db", sc_db_kind_name(db_config->kind));
    log.data = db_field;
    log.data_count = 1;

    status = sc_db_open_waiting(db_config, quit, &out->db);
    if (status != SC_OK) {
        /* The driver's own sentence is already on the db.connection.failed lines above this one;
         * this is the line that says the run is over. */
        sc_log_event(SC_LOG_FATAL, SC_CAT_STARTUP, "startup.database.failed", &log,
                     "cannot reach the database");
        bc_context_close(out);
        return status;
    }

    status = bc_migrations_run(out->db, NULL);
    if (status != SC_OK) {
        /* A schema this build cannot run against was already reported as db.migration.denied,
         * with the migration named and what to do about it. Saying it again under a heading
         * about reaching the database would only make the useful line harder to find. */
        if (status != SC_ERR_MALFORMED)
            sc_log_event(SC_LOG_FATAL, SC_CAT_STARTUP, "startup.database.failed", &log,
                         "the database could not be migrated");
        bc_context_close(out);
        return status;
    }

    status = bc_community_find_home(out->db, &out->home, &found, error, sizeof(error));
    if (status != SC_OK) {
        sc_log_event(SC_LOG_FATAL, SC_CAT_STARTUP, "startup.database.failed", &log,
                     "cannot read the home community: %s", error);
        bc_context_close(out);
        return status;
    }
    if (found)
        return SC_OK;

    /*
     * There is no second case here and that is the point. There is no third one either, because
     * `users.community_id` is NOT NULL: without this row nothing can register, so serving
     * without it would only mean failing later and less clearly.
     */
    {
        sc_log_value reason[1] = {SC_LOG_STR("reason", "not-set-up")};
        sc_log_context setup_log = {0};

        setup_log.data = reason;
        setup_log.data_count = 1;
        sc_log_event(SC_LOG_FATAL, SC_CAT_STARTUP, "startup.setup.failed", &setup_log,
                     "cannot start: this database has no community yet. Run the setup command "
                     "once, with a terminal attached, to say who this community is -- under "
                     "docker compose that is: docker compose run --rm backend setup");
    }
    bc_context_close(out);
    return SC_ERR_UNAVAILABLE;
}

void bc_context_close(bc_context *context)
{
    if (context == NULL)
        return;
    sc_db_close(context->db);
    context->db = NULL;
    /* Every role thread has been joined by the time a role closes its context, so nothing can
     * be waiting on this. */
    uv_mutex_destroy(&context->db_lock);
}

void bc_context_lock(bc_context *context)
{
    uv_mutex_lock(&context->db_lock);
}

void bc_context_unlock(bc_context *context)
{
    uv_mutex_unlock(&context->db_lock);
}
