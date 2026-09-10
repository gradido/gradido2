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
 * The calls below block, and every caller is a thread for which that is right: a startup that
 * has nothing else to do until the database answers, and the database workers, each of which
 * owns one connection and exists to wait on it (service_core/db_exec.h). No event loop ever
 * calls into libpq -- Architecture.md, *Threading*, says why the asynchronous form on the loop
 * was measured and not chosen.
 */
#include "service_core/db.h"

#include "db_internal.h"

#if defined(SC_DB_WITH_POSTGRESQL)

#include <poll.h>
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
    /* The session every statement is read in: timestamps come back in UTC and in ISO form,
     * which is the one shape sql.c parses, and text is UTF-8 whatever the server's locale is.
     * Set at connect rather than with a SET afterwards, so it costs no round trip and holds
     * again after a reset. */
    keys[7] = "options";
    values[7] = "-c TimeZone=UTC -c DateStyle=ISO";
    keys[8] = "client_encoding";
    values[8] = "UTF8";
    keys[9] = NULL;
    values[9] = NULL;
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

/** See the call site: a notice is not an error and stderr is not free-form here. */
static void discard_notice(void *arg, const char *message)
{
    (void)arg;
    (void)message;
}

sc_status sc_db_postgres_open(const sc_db_config *cfg, sc_db *db)
{
    const char *keys[10];
    const char *values[10];
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
    /*
     * libpq's default notice processor writes to stderr, and this process writes one JSON object
     * per line there -- a German sentence about `CREATE TABLE IF NOT EXISTS` skipping a relation
     * lands in the middle of the log stream and stops it being parseable. So the notices are
     * dropped, which is also what the TypeScript path's driver does with them.
     *
     * Dropped rather than logged, and that is a decision rather than laziness: the event
     * vocabulary in contracts/logging.json is closed, there is no event for "the database
     * remarked on something", and inventing one on this path would be a line the other
     * implementation never writes. If a notice ever has to be seen, it gets a contracted event
     * first.
     */
    PQsetNoticeProcessor(conn, discard_notice, NULL);
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

/** Whether the socket has anything to read, without waiting for it. */
static int has_input(PGconn *conn)
{
    struct pollfd p;

    p.fd = PQsocket(conn);
    p.events = POLLIN;
    p.revents = 0;
    return p.fd >= 0 && poll(&p, 1, 0) > 0;
}

/* A server that keeps talking to a connection nobody is using is not one this waits out. */
#define REVIVE_READS_MAX 8

/*
 * A connection the server closed is not known to be closed until something reads from it --
 * PQstatus says CONNECTION_OK right up to the statement that fails. After a PostgreSQL restart
 * that would be one failed request per pooled connection, each of them a 500 for a database that
 * is by then perfectly fine.
 *
 * So it is read before it is handed out, and the reading has to go on until the socket is quiet,
 * because one read is not enough -- measured, not assumed: a terminated session sends its FATAL
 * message and then closes, and on an idle connection libpq hands that message to the notice
 * processor and leaves the status alone. Only the read *after* it meets the end of the stream,
 * and with one read that was the request's own: "SSL connection has been closed unexpectedly".
 *
 * An idle connection has nothing to say, so a socket that is readable at all is a server that
 * said something, and it is read out: PQconsumeInput to take it in, PQisBusy to parse it. Neither
 * blocks -- libpq keeps the socket non-blocking whatever PQsetnonblocking says -- and on a healthy
 * connection the whole check is one poll() that answers "nothing".
 *
 * A connection that came out of that CONNECTION_BAD is dialled again, once. This does not wait
 * for a server that is still coming back up: that is a failure for the one request that met it,
 * and the next hand-out tries again.
 */
sc_status sc_db_postgres_revive(sc_db *db, int *revived)
{
    PGconn *conn = (PGconn *)db->native;
    int reads = 0;

    *revived = 0;
    if (conn == NULL) {
        sc_db_set_error(db, "no connection");
        return SC_ERR_NETWORK;
    }
    while (PQstatus(conn) == CONNECTION_OK && reads++ != REVIVE_READS_MAX && has_input(conn)) {
        if (!PQconsumeInput(conn))
            break;
        (void)PQisBusy(conn);
    }
    if (PQstatus(conn) == CONNECTION_OK)
        return SC_OK;

    PQreset(conn);
    if (PQstatus(conn) != CONNECTION_OK) {
        sc_db_set_error(db, PQerrorMessage(conn));
        return SC_ERR_NETWORK;
    }
    /* A reset connection is a new session on the same connection object. The notice processor
     * belongs to the object and survives; the prepared statements belonged to the session and
     * did not, so the table that remembers them is emptied with it. */
    sc_sql_postgres_forget(db);
    *revived = 1;
    return SC_OK;
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

sc_status sc_db_postgres_revive(sc_db *db, int *revived)
{
    *revived = 0;
    sc_db_set_error(db, "this build has no PostgreSQL driver");
    return SC_ERR_UNAVAILABLE;
}

void sc_db_postgres_close(sc_db *db)
{
    db->native = NULL;
}

#endif /* SC_DB_WITH_POSTGRESQL */
