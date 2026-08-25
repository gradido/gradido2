/*
 * The part of the database connection that does not depend on which one it is: reading the
 * environment, choosing the backend, and waiting for a database that is still starting.
 *
 * db.h holds the design. What is worth repeating here is the one rule this file implements
 * rather than describes -- a failure is either "not yet" or "not like this", and only the first
 * is worth waiting for. Everything else is dispatch.
 */
#include "service_core/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db_internal.h"
#include "service_core/log.h"
#include "service_core/runtime.h"

/* The TypeScript path's defaults, from packages/backend-core/src/database/schema.ts. They are
 * mirrored rather than chosen: backend and federation reach the same database from two
 * implementations, and a default that differs between them is a community connecting to two. */
#define DEFAULT_HOST "localhost"
#define DEFAULT_PORT 5432
#define DEFAULT_USER "gradido"
#define DEFAULT_DATABASE "gradido_community"
#define DEFAULT_FILE "./gradido_community.sqlite"

static sc_status copy_env(char *dst, size_t dst_size, const char *name, const char *fallback)
{
    const char *value = getenv(name);
    size_t len;

    if (value == NULL)
        value = fallback;
    len = strlen(value);
    if (len >= dst_size) {
        sc_log_fatal(SC_CAT_STARTUP, "config.value_too_long", "%s is %zu bytes, the limit is %zu",
                     name, len, dst_size - 1);
        return SC_ERR_TOO_LONG;
    }
    memcpy(dst, value, len + 1);
    return SC_OK;
}

static sc_status read_port(uint16_t *out, const char *name, uint16_t fallback)
{
    const char *value = getenv(name);
    char *end;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        *out = fallback;
        return SC_OK;
    }
    parsed = strtoul(value, &end, 10);
    if (*end != '\0' || parsed == 0 || parsed > 65535) {
        sc_log_fatal(SC_CAT_STARTUP, "config.port_invalid",
                     "%s is '%s', which is not a port between 1 and 65535", name, value);
        return SC_ERR_MALFORMED;
    }
    *out = (uint16_t)parsed;
    return SC_OK;
}

const char *sc_db_kind_name(sc_db_kind kind)
{
    switch (kind) {
    case SC_DB_POSTGRESQL:
        return "postgresql";
    case SC_DB_SQLITE:
        return "sqlite";
    }
    return "unknown";
}

int sc_db_kind_available(sc_db_kind kind)
{
    switch (kind) {
    case SC_DB_POSTGRESQL:
        return sc_db_postgres_available();
    case SC_DB_SQLITE:
        return sc_db_sqlite_available();
    }
    return 0;
}

const char *sc_db_drivers(void)
{
    if (sc_db_postgres_available() && sc_db_sqlite_available())
        return "postgresql, sqlite";
    if (sc_db_postgres_available())
        return "postgresql";
    if (sc_db_sqlite_available())
        return "sqlite";
    return "none";
}

/** The build option that would have provided @p kind, for the line that says what is missing. */
static const char *build_option_for(sc_db_kind kind)
{
    return kind == SC_DB_POSTGRESQL ? "-Dpostgres" : "-Dsqlite";
}

sc_status sc_db_config_load(sc_db_config *out)
{
    const char *type = getenv("DB_TYPE");
    sc_status status;

    if (out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (type == NULL || type[0] == '\0' || strcmp(type, "postgresql") == 0) {
        out->kind = SC_DB_POSTGRESQL;
    } else if (strcmp(type, "sqlite") == 0) {
        out->kind = SC_DB_SQLITE;
    } else {
        sc_log_fatal(SC_CAT_STARTUP, "config.database_type_invalid",
                     "DB_TYPE is '%s', which is neither postgresql nor sqlite", type);
        return SC_ERR_MALFORMED;
    }

    status = copy_env(out->host, sizeof(out->host), "DB_HOST", DEFAULT_HOST);
    if (status != SC_OK)
        return status;
    status = copy_env(out->user, sizeof(out->user), "DB_USER", DEFAULT_USER);
    if (status != SC_OK)
        return status;
    status = copy_env(out->password, sizeof(out->password), "DB_PASSWORD", "");
    if (status != SC_OK)
        return status;
    status = copy_env(out->database, sizeof(out->database), "DB_DATABASE", DEFAULT_DATABASE);
    if (status != SC_OK)
        return status;
    status = copy_env(out->file, sizeof(out->file), "DB_FILE", DEFAULT_FILE);
    if (status != SC_OK)
        return status;
    status = read_port(&out->port, "DB_PORT", DEFAULT_PORT);
    if (status != SC_OK)
        return status;

    /*
     * "an empty database password is not acceptable in production" -- the same rule, read from
     * the same variable, as packages/backend-core/src/database/schema.ts. NODE_ENV is an odd
     * thing for a C binary to consult and it is still the right one: the two implementations
     * are deployed into one environment and must agree on when it is production.
     *
     * It fits PostgreSQL over TCP, which is what DB_HOST defaults to. It does not fit peer
     * authentication over a Unix socket, where no password is the correct configuration and
     * this refuses to start -- so does the TypeScript path, and that is where the exemption has
     * to be decided if it is ever wanted. Not here: a rule that is stricter on one path than
     * the other is a rule an operator learns twice.
     */
    if (out->kind == SC_DB_POSTGRESQL && out->password[0] == '\0') {
        const char *node_env = getenv("NODE_ENV");
        if (node_env != NULL && strcmp(node_env, "production") == 0) {
            sc_log_fatal(SC_CAT_STARTUP, "config.database_password_empty",
                         "DB_PASSWORD is empty and NODE_ENV is production");
            return SC_ERR_MALFORMED;
        }
    }

    return SC_OK;
}

void sc_db_config_log(const sc_db_config *cfg)
{
    if (cfg == NULL)
        return;
    if (cfg->kind == SC_DB_SQLITE) {
        sc_log_info(SC_CAT_STARTUP, "config.database", "sqlite, file %s", cfg->file);
        return;
    }
    /* The password is reported as present or absent. Printing it would put the database's
     * credentials into every log aggregator the operator happens to run. */
    sc_log_info(SC_CAT_STARTUP, "config.database",
                "postgresql, host %s, port %u, database %s, user %s, password %s", cfg->host,
                (unsigned)cfg->port, cfg->database, cfg->user,
                cfg->password[0] != '\0' ? "set" : "(unset)");
}

void sc_db_set_error(sc_db *db, const char *message)
{
    size_t i;
    size_t out;

    if (db == NULL)
        return;
    if (message == NULL || message[0] == '\0') {
        memcpy(db->error, "the driver reported no reason", sizeof("the driver reported no reason"));
        return;
    }
    /* libpq ends its messages with a newline and puts its hint on a further line, indented. A
     * log line is a line, so the whole message becomes one -- and a run of whitespace becomes
     * one space rather than the four that "\n\t" and friends would otherwise leave in it. */
    out = 0;
    for (i = 0; out + 1 < sizeof(db->error) && message[i] != '\0'; ++i) {
        char c = message[i];
        int is_space = (c == ' ' || c == '\n' || c == '\r' || c == '\t');

        if (is_space) {
            if (out == 0 || db->error[out - 1] == ' ')
                continue;
            c = ' ';
        }
        db->error[out++] = c;
    }
    if (out > 0 && db->error[out - 1] == ' ')
        --out;
    db->error[out] = '\0';
}

/**
 * One attempt, without a word about it.
 *
 * The reason is copied into @p reason rather than logged, because the same failure means two
 * different things depending on who asked: a single sc_db_open() that fails is an error, and
 * the twenty-ninth attempt of a startup wait is a database that has not finished starting. The
 * caller knows which it is; this does not, and a line written here would be the wrong one half
 * the time -- or, worse, both lines for one failure.
 */
static sc_status open_once(const sc_db_config *cfg, sc_db **out, char *reason, size_t reason_size)
{
    sc_db *db;
    sc_status status;

    db = (sc_db *)calloc(1, sizeof(*db));
    if (db == NULL) {
        (void)snprintf(reason, reason_size, "out of memory");
        return SC_ERR_NO_MEMORY;
    }
    db->kind = cfg->kind;

    status =
        cfg->kind == SC_DB_POSTGRESQL ? sc_db_postgres_open(cfg, db) : sc_db_sqlite_open(cfg, db);
    if (status != SC_OK) {
        /* Out of the handle before it is freed: after this there is nothing left to ask. */
        (void)snprintf(reason, reason_size, "%s", db->error);
        sc_db_close(db);
        return status;
    }
    *out = db;
    return SC_OK;
}

/** The check both entry points make first, and the line it writes when it fails. */
static sc_status check_driver(const sc_db_config *cfg)
{
    if (sc_db_kind_available(cfg->kind))
        return SC_OK;
    sc_log_fatal(SC_CAT_STARTUP, "database.driver_missing",
                 "DB_TYPE is %s and this build has no driver for it -- it was built with "
                 "%s=false; drivers in this binary: %s",
                 sc_db_kind_name(cfg->kind), build_option_for(cfg->kind), sc_db_drivers());
    return SC_ERR_UNAVAILABLE;
}

sc_status sc_db_open(const sc_db_config *cfg, sc_db **out)
{
    char reason[SC_DB_ERROR_MAX];
    sc_status status;

    if (cfg == NULL || out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    status = check_driver(cfg);
    if (status != SC_OK)
        return status;

    status = open_once(cfg, out, reason, sizeof(reason));
    if (status != SC_OK) {
        sc_log_error(SC_CAT_DB, "db.connection.failed", "%s: %s", sc_db_kind_name(cfg->kind),
                     reason);
        return status;
    }
    sc_log_info(SC_CAT_DB, "db.connection.opened", "%s", sc_db_kind_name(cfg->kind));
    return SC_OK;
}

sc_status sc_db_probe(sc_db *db)
{
    if (db == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    return db->kind == SC_DB_POSTGRESQL ? sc_db_postgres_probe(db) : sc_db_sqlite_probe(db);
}

/** Waits out @p delay_ms in ticks, so a shutdown is noticed as quickly here as anywhere else.
 *  Answers non-zero when it was cut short by one. */
static int wait_or_quit(int64_t delay_ms, const sc_quit_flag *quit)
{
    int64_t waited = 0;

    while (waited < delay_ms && !sc_quit_requested(quit)) {
        int64_t step = delay_ms - waited;
        if (step > SC_RUNTIME_TICK_MS)
            step = SC_RUNTIME_TICK_MS;
        sc_runtime_sleep_ms((unsigned int)step);
        waited += step;
    }
    return sc_quit_requested(quit);
}

sc_status sc_db_open_waiting(const sc_db_config *cfg, const sc_quit_flag *quit, sc_db **out)
{
    char reason[SC_DB_ERROR_MAX];
    uint32_t attempts;
    int64_t delay_ms;
    uint32_t attempt;
    sc_status status;

    if (cfg == NULL || out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    status = check_driver(cfg);
    if (status != SC_OK)
        return status;

    attempts = cfg->connect_attempts != 0 ? cfg->connect_attempts : SC_DB_CONNECT_ATTEMPTS_DEFAULT;
    delay_ms = cfg->connect_delay_ms != 0 ? cfg->connect_delay_ms : SC_DB_CONNECT_DELAY_DEFAULT_MS;
    /* SQLite is opened when the connection is created, so there is no second party to wait for:
     * a failure now is a broken or unreadable file and will still be one in a second. */
    if (cfg->kind == SC_DB_SQLITE)
        attempts = 1;

    reason[0] = '\0';
    for (attempt = 1; attempt <= attempts; ++attempt) {
        status = open_once(cfg, out, reason, sizeof(reason));
        if (status == SC_OK) {
            sc_log_info(SC_CAT_DB, "db.connection.opened", "%s", sc_db_kind_name(cfg->kind));
            return SC_OK;
        }
        /* Only "the database did not answer" is worth another attempt. A wrong password, a
         * database that does not exist and a refusal by pg_hba.conf all answer something else,
         * and waiting does not turn any of them into a connection. */
        if (status != SC_ERR_NETWORK)
            break;
        if (attempt == attempts)
            break;
        /* One line per attempt, at warn: during a startup wait this is the expected case, and
         * thirty error lines for a database that came up in four seconds is a log that has
         * cried wolf. The failure that ends the wait is the one logged at error, below. */
        sc_log_warn(SC_CAT_DB, "db.connection.failed",
                    "database not reachable yet, attempt %u of %u, retrying in %lld ms: %s",
                    (unsigned)attempt, (unsigned)attempts, (long long)delay_ms, reason);
        if (wait_or_quit(delay_ms, quit)) {
            sc_log_info(SC_CAT_DB, "db.connection.failed",
                        "shutdown requested while waiting for the database");
            return SC_ERR_NETWORK;
        }
    }
    sc_log_error(SC_CAT_DB, "db.connection.failed", "%s: %s", sc_db_kind_name(cfg->kind), reason);
    return status;
}

void sc_db_close(sc_db *db)
{
    if (db == NULL)
        return;
    if (db->native != NULL) {
        if (db->kind == SC_DB_POSTGRESQL)
            sc_db_postgres_close(db);
        else
            sc_db_sqlite_close(db);
    }
    free(db);
}

sc_db_kind sc_db_kind_of(const sc_db *db)
{
    return db != NULL ? db->kind : SC_DB_POSTGRESQL;
}

void *sc_db_native(sc_db *db)
{
    return db != NULL ? db->native : NULL;
}

const char *sc_db_error(const sc_db *db)
{
    return db != NULL ? db->error : "";
}
