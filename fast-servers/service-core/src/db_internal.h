/*
 * What db.c and the two backends share, and nothing above service-core ever sees.
 *
 * The split is the same one http_common.c / http_h2o.c / http_fallback.c already use: the part
 * that is the same whichever driver answered lives in one file, and each driver implements the
 * three calls below. Unlike the HTTP backends both of these are compiled -- which database is
 * used is a startup decision, not a build one -- so each of them also has an *absent* form,
 * compiled when the build was told to leave that driver out, and that form is where
 * SC_ERR_UNAVAILABLE comes from.
 */
#ifndef SERVICE_CORE_DB_INTERNAL_H
#define SERVICE_CORE_DB_INTERNAL_H

#include "service_core/db.h"
#include "service_core/sql.h"

/* Long enough for what libpq says about a refused connection, which is a sentence and a hint. */
#define SC_DB_ERROR_MAX 512

struct sc_db {
    sc_db_kind kind;
    /* PGconn * or sqlite3 *. Owned by the backend that opened it. */
    void *native;
    /* The last driver message, already collapsed onto one line. */
    char error[SC_DB_ERROR_MAX];
    /*
     * This connection's prepared statements, by sc_sql_statement slot. On SQLite the
     * sqlite3_stmt itself; on PostgreSQL a marker that PQprepare has run for that slot on this
     * session -- the statement is named by the slot, so there is nothing else to keep. Emptied
     * whenever the session is: a reset PostgreSQL connection has forgotten every name.
     */
    void *prepared[SC_SQL_STATEMENTS_MAX];
};

/**
 * Copies @p message into @p db's error buffer, as one line and without a trailing newline --
 * libpq ends every message with one and a log line is a line.
 *
 * NULL or an empty message leaves a sentence saying the driver gave none, so that a caller
 * printing sc_db_error() never prints nothing at all. A message that does not fit is truncated:
 * this is a diagnostic, and the rule that a truncated value is worse than a refused one is
 * about values that connect somewhere or deliver to someone.
 */
void sc_db_set_error(sc_db *db, const char *message);

/*
 * The backends. Each is implemented twice in its own file, once for the build that has the
 * driver and once for the build that does not.
 *
 * open() fills db->native and answers, in the same vocabulary sc_db_open() documents:
 * SC_ERR_UNAVAILABLE when the driver is not in this build, SC_ERR_NETWORK for a database that
 * did not answer, SC_ERR_INVALID_ARGUMENT for one that answered and refused. That distinction
 * is the whole of what sc_db_open_waiting() needs to tell "not yet" from "not like this".
 */
sc_status sc_db_postgres_open(const sc_db_config *cfg, sc_db *db);
sc_status sc_db_postgres_probe(sc_db *db);
/* Notices a connection the server has closed and dials it again -- see db_exec.c, where a worker
 * calls it before every unit. SC_OK for a connection that is fine or was brought back,
 * SC_ERR_NETWORK for one that stayed gone, with the reason in db->error. */
sc_status sc_db_postgres_revive(sc_db *db, int *revived);
void sc_db_postgres_close(sc_db *db);
int sc_db_postgres_available(void);

sc_status sc_db_sqlite_open(const sc_db_config *cfg, sc_db *db);
sc_status sc_db_sqlite_probe(sc_db *db);
void sc_db_sqlite_close(sc_db *db);
int sc_db_sqlite_available(void);

/* --- statements, per driver -- see sql.c for the part that is the same on both -------- */

/* One line, no trailing newline, into @p error->message, and the kind. */
void sc_sql_set_error(sc_sql_error *error, sc_sql_error_kind kind, const char *message);

sc_status sc_sql_postgres_run(sc_db *db, sc_sql_statement *statement, int32_t slot,
                              const sc_sql_param *params, uint32_t param_count, sc_sql_rows *rows,
                              int64_t *changes, sc_sql_error *error);
int sc_sql_postgres_next(sc_sql_rows *rows);
void sc_sql_postgres_close(sc_sql_rows *rows);
sc_status sc_sql_postgres_simple(sc_db *db, const char *text, sc_sql_error *error);
int sc_sql_postgres_is_null(const sc_sql_rows *rows, uint32_t column);
const char *sc_sql_postgres_text(const sc_sql_rows *rows, uint32_t column, uint32_t *size);
/* Called when a session ends or is replaced. */
void sc_sql_postgres_forget(sc_db *db);

sc_status sc_sql_sqlite_run(sc_db *db, sc_sql_statement *statement, int32_t slot,
                            const sc_sql_param *params, uint32_t param_count, sc_sql_rows *rows,
                            int64_t *changes, sc_sql_error *error);
int sc_sql_sqlite_next(sc_sql_rows *rows);
void sc_sql_sqlite_close(sc_sql_rows *rows);
sc_status sc_sql_sqlite_simple(sc_db *db, const char *text, sc_sql_error *error);
int sc_sql_sqlite_is_null(const sc_sql_rows *rows, uint32_t column);
int64_t sc_sql_sqlite_int(const sc_sql_rows *rows, uint32_t column);
const char *sc_sql_sqlite_text(const sc_sql_rows *rows, uint32_t column, uint32_t *size);
int64_t sc_sql_sqlite_bytes(const sc_sql_rows *rows, uint32_t column, uint8_t *out,
                            size_t out_size);
/* Finalizes every prepared statement; before the connection is closed. */
void sc_sql_sqlite_forget(sc_db *db);

#endif /* SERVICE_CORE_DB_INTERNAL_H */
