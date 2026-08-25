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

/* Long enough for what libpq says about a refused connection, which is a sentence and a hint. */
#define SC_DB_ERROR_MAX 512

struct sc_db {
    sc_db_kind kind;
    /* PGconn * or sqlite3 *. Owned by the backend that opened it. */
    void *native;
    /* The last driver message, already collapsed onto one line. */
    char error[SC_DB_ERROR_MAX];
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
void sc_db_postgres_close(sc_db *db);
int sc_db_postgres_available(void);

sc_status sc_db_sqlite_open(const sc_db_config *cfg, sc_db *db);
sc_status sc_db_sqlite_probe(sc_db *db);
void sc_db_sqlite_close(sc_db *db);
int sc_db_sqlite_available(void);

#endif /* SERVICE_CORE_DB_INTERNAL_H */
