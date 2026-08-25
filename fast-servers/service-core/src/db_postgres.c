/*
 * PostgreSQL, through libpq.
 *
 * Architecture.md, *Databases*, holds why it is libpq and not a protocol client of this
 * project's own: of the 48,1 us one uncached request spends on the database, 3,6 are user-space
 * CPU, and that is the entire budget a different client could compete for -- against the wire
 * protocol, TLS, SCRAM and failover it would have to take on to spend it.
 *
 * Two rules from the same section are not implemented here and are not forgotten either. Both
 * belong to the request path, and this file is startup:
 *
 *   Unix socket, never TCP loopback, when the database is on this host. 83,4 -> 48,1 us. It is
 *   a *configuration*: DB_HOST starting with '/' is a socket directory to libpq, so the rule
 *   costs no code here and is written down in db.h where an operator setting DB_HOST will see
 *   it.
 *
 *   One round trip per request -- user row and roles in one statement, not two. That is query
 *   construction, it is business logic wearing SQL, and it arrives with the first repository.
 *
 * What is genuinely missing is the asynchronous form: PQsocket / PQconsumeInput / PQisBusy on
 * h2o's loop, which is how a request will reach the database without occupying a thread while
 * it waits. The calls below block, which is correct for the one caller they have -- a startup
 * that has nothing else to do until the database answers.
 */
#include "service_core/db.h"

#include "db_internal.h"

#if defined(SC_DB_WITH_POSTGRESQL)

#include <stdio.h>

#include <libpq-fe.h>

int sc_db_postgres_available(void)
{
    return 1;
}

/* Keyword arrays rather than a connection string, so that a password containing a space or a
 * quote is a password and not a parse error waiting for the one operator who picks that
 * character. libpq does no unescaping on these. */
static void fill_params(const sc_db_config *cfg, const char **keys, const char **values,
                        char *port_text, size_t port_size, char *timeout_text, size_t timeout_size)
{
    int64_t timeout_ms =
        cfg->connect_timeout_ms != 0 ? cfg->connect_timeout_ms : SC_DB_CONNECT_TIMEOUT_DEFAULT_MS;
    /* libpq counts this in seconds and reads 0 as "wait forever", which is the one value this
     * must never pass on: a host that answers nothing would hold the startup for minutes. */
    long long timeout_s = (long long)((timeout_ms + 999) / 1000);
    if (timeout_s < 1)
        timeout_s = 1;

    (void)snprintf(port_text, port_size, "%u", (unsigned)cfg->port);
    (void)snprintf(timeout_text, timeout_size, "%lld", timeout_s);

    keys[0] = "host";
    values[0] = cfg->host;
    keys[1] = "port";
    values[1] = port_text;
    keys[2] = "user";
    values[2] = cfg->user;
    keys[3] = "password";
    values[3] = cfg->password;
    keys[4] = "dbname";
    values[4] = cfg->database;
    keys[5] = "connect_timeout";
    values[5] = timeout_text;
    /* Names this implementation in pg_stat_activity. Two implementations share one database;
     * which of them is holding a connection is worth being able to see without guessing. */
    keys[6] = "application_name";
    values[6] = "fast-servers";
    keys[7] = NULL;
    values[7] = NULL;
}

/**
 * Tells "not yet" from "not like this" for a connection that failed.
 *
 * The TypeScript path reads the SQLSTATE the driver kept -- classes 28, 3D and 42 are the
 * server saying it heard the question and refused it. libpq exposes no SQLSTATE for a
 * *connection* failure, so this asks the other question it does answer: PQping says whether
 * anything is listening at all.
 *
 *   PQPING_OK           something is there and accepting connections, so the failure was ours:
 *                       wrong password, no such database, no permission. Retrying does not turn
 *                       a wrong password into a right one.
 *   PQPING_REJECT       alive but not accepting yet -- a server replaying its write-ahead log
 *                       says exactly this, and it is the case worth waiting for.
 *   PQPING_NO_RESPONSE  nothing answered: refused, unresolvable, timed out. What a database
 *                       that has not been started yet looks like from here.
 *   PQPING_NO_ATTEMPT   libpq would not even try, which is a parameter it cannot use.
 *
 * It costs one extra round trip on a failed startup connection and nothing at all on a
 * successful one.
 */
static sc_status classify_failure(const char *const *keys, const char *const *values)
{
    switch (PQpingParams(keys, values, 0)) {
    case PQPING_OK:
    case PQPING_NO_ATTEMPT:
        return SC_ERR_INVALID_ARGUMENT;
    case PQPING_REJECT:
    case PQPING_NO_RESPONSE:
    default:
        return SC_ERR_NETWORK;
    }
}

sc_status sc_db_postgres_open(const sc_db_config *cfg, sc_db *db)
{
    const char *keys[8];
    const char *values[8];
    char port_text[6];
    char timeout_text[16];
    PGconn *conn;

    fill_params(cfg, keys, values, port_text, sizeof(port_text), timeout_text,
                sizeof(timeout_text));

    conn = PQconnectdbParams(keys, values, 0);
    if (conn == NULL) {
        sc_db_set_error(db, "libpq could not allocate a connection");
        return SC_ERR_NO_MEMORY;
    }
    if (PQstatus(conn) != CONNECTION_OK) {
        sc_db_set_error(db, PQerrorMessage(conn));
        PQfinish(conn);
        return classify_failure(keys, values);
    }
    db->native = conn;
    return SC_OK;
}

sc_status sc_db_postgres_probe(sc_db *db)
{
    PGconn *conn = (PGconn *)db->native;
    PGresult *result;
    sc_status status = SC_OK;

    if (conn == NULL) {
        sc_db_set_error(db, "no connection");
        return SC_ERR_INVALID_ARGUMENT;
    }
    result = PQexec(conn, "select 1");
    if (result == NULL) {
        sc_db_set_error(db, PQerrorMessage(conn));
        return SC_ERR_NETWORK;
    }
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        sc_db_set_error(db, PQerrorMessage(conn));
        /* A connection that is gone is a different failure from a database that answered
         * something unexpected, and the caller may retry only the first. */
        status = PQstatus(conn) == CONNECTION_BAD ? SC_ERR_NETWORK : SC_ERR_MALFORMED;
    }
    PQclear(result);
    return status;
}

void sc_db_postgres_close(sc_db *db)
{
    PQfinish((PGconn *)db->native);
    db->native = NULL;
}

#else /* the build was told to leave this driver out */

/* NULL, and nothing else -- the driver's own header is what carried it in the branch above. */
#include <stddef.h>

int sc_db_postgres_available(void)
{
    return 0;
}

/*
 * Reached only when sc_db_open() was called for a kind sc_db_kind_available() says this build
 * does not have -- which it checks first, and logs. These exist so that the dispatch in db.c
 * needs no #if of its own, and so that a caller reaching one directly still gets an answer
 * rather than a link error.
 */
sc_status sc_db_postgres_open(const sc_db_config *cfg, sc_db *db)
{
    (void)cfg;
    sc_db_set_error(db, "this build has no PostgreSQL driver; it was built with -Dpostgres=false");
    return SC_ERR_UNAVAILABLE;
}

sc_status sc_db_postgres_probe(sc_db *db)
{
    sc_db_set_error(db, "this build has no PostgreSQL driver");
    return SC_ERR_UNAVAILABLE;
}

void sc_db_postgres_close(sc_db *db)
{
    db->native = NULL;
}

#endif /* SC_DB_WITH_POSTGRESQL */
