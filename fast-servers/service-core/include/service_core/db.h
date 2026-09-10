/*
 * The database connection: which one, opened, waited for, closed.
 *
 * Two of them, and which is used is decided in the environment at startup, never here --
 * `../Architecture.md`, *DB*: PostgreSQL is the reference and the default for server mode,
 * SQLite is what makes a small community's installation a download-and-start affair. The
 * variables are the ones the TypeScript path reads, down to the spelling, because backend and
 * federation talk to the same database and must name it the same way:
 * `packages/backend-core/src/database/schema.ts` is the original of the struct below.
 *
 *   PostgreSQL   libpq, built from a pinned postgres checkout
 *   SQLite       the amalgamation, in this process, called directly
 *
 * ### What this surface is not
 *
 * It is not a driver abstraction, and that is deliberate rather than unfinished. The TypeScript
 * side spells the same decision out where it declares its connection as a discriminated union:
 * the SQL dialects are not the same, and a repository that has to know which one it is talking
 * to should have to say so. So there is no sc_db_query() here that both backends implement --
 * what is shared is opening, probing, waiting and closing, which genuinely are the same
 * question asked twice. A statement is written against the driver, reached through
 * sc_db_native(), by code that already knows the dialect it is in.
 *
 * service_core/sql.h is where statements are run, and it keeps the part of this that matters: a
 * statement carries its text for each dialect, written by the repository, and what the two
 * share is binding, stepping and reading -- not the SQL.
 *
 * The row mapping under that is generated from the table contracts in `contracts/db/` rather
 * than written by hand -- 330 columns across 29 tables, where a wrong column index is a silent
 * wrong amount rather than a compile error. `Architecture.md`, *The mapping is generated, not
 * written*, is normative for it, and nothing in this header anticipates its shape beyond
 * keeping the driver reachable.
 *
 * ### One connection here, statements in sql.h, where they run in db_exec.h
 *
 * This header opens *a* connection, which is what startup, migrations and the setup command
 * need. Statements are service_core/sql.h. A serving role reaches the database through
 * service_core/db_exec.h, whose workers each own one of these connections for their whole life
 * and run the requests' work on it -- so everything here may block, and does, on a thread that
 * is not an event loop.
 *
 * ### A driver the build left out
 *
 * Both are compiled in by default. `-Dpostgres=false` and `-Dsqlite=false` leave one out, and
 * then asking for that database answers SC_ERR_UNAVAILABLE at sc_db_open() -- before anything
 * has been dialled, with a log line naming the option that would have provided it. It is not an
 * error to *have* built without a driver; it is an error to ask that build for that database.
 */
#ifndef SERVICE_CORE_DB_H
#define SERVICE_CORE_DB_H

#include <stdint.h>

#include "service_core/runtime.h"
#include "service_core/status.h"

/*
 * Sizes. Every string is a fixed-size buffer and a value that does not fit refuses the startup
 * rather than truncating -- half a host name is a host name, and it is the wrong one.
 */
/* Long enough for a name, an address, and the directory of a Unix socket -- see the host field. */
#define SC_DB_HOST_MAX 256
#define SC_DB_USER_MAX 64
#define SC_DB_PASSWORD_MAX 128
#define SC_DB_NAME_MAX 64
/* A path, so it is the one field here that may legitimately be long. */
#define SC_DB_FILE_MAX 512

/** Attempts a PostgreSQL connection is given before the startup is called failed. */
#define SC_DB_CONNECT_ATTEMPTS_DEFAULT 30
/** Pause between them. Thirty seconds in total covers a database replaying its write-ahead log
 *  and a compose stack that has not started it yet, and is short enough that a genuinely absent
 *  database is reported while someone is still watching. */
#define SC_DB_CONNECT_DELAY_DEFAULT_MS 1000
/** How long one attempt may take. A refused connection comes back in milliseconds; this is for
 *  the host that answers nothing at all, where the socket would otherwise sit for minutes. */
#define SC_DB_CONNECT_TIMEOUT_DEFAULT_MS 5000

/**
 * DB_POOL_SIZE when nothing sets it -- the same ten bun's SQL client opens by default, so the two
 * implementations hold the same number against one database until somebody decides otherwise.
 *
 * Ten is not a measurement of anything, and it cannot be one: how many statements a PostgreSQL
 * server can usefully run at once is a property of *that* server, which this process knows
 * nothing about. What ten is, is safe -- a stock server allows 100 connections, and ten per
 * serving process leaves room for both roles, the other implementation and an administrator.
 * A database that can do more is told so. contracts/database-config.json, rules.pool.
 */
#define SC_DB_POOL_SIZE_DEFAULT 10

typedef enum sc_db_kind {
    /* The reference. DB_TYPE=postgresql, which is also the default. */
    SC_DB_POSTGRESQL = 0,
    /* DB_TYPE=sqlite. Opened when the connection is created, so there is no second party to
     * wait for and no retry: a failure now is a broken or unreadable file and will still be one
     * in a second. */
    SC_DB_SQLITE = 1
} sc_db_kind;

/*
 * What the environment says. The field comments name the variable each one comes from; the
 * defaults are the TypeScript path's defaults and are checked against it, not chosen here.
 */
typedef struct sc_db_config {
    sc_db_kind kind; /* DB_TYPE, "postgresql" or "sqlite" */

    /*
     * PostgreSQL only.
     *
     * A value that starts with '/' is a directory holding a Unix socket rather than a host
     * name, which is libpq's own convention and the one this project wants: 83,4 to 48,1 us
     * for one connection string when the database is on this machine. Architecture.md,
     * *Where a query's time goes*, has the measurement.
     */
    char host[SC_DB_HOST_MAX];         /* DB_HOST, default "localhost" */
    uint16_t port;                     /* DB_PORT, default 5432 */
    char user[SC_DB_USER_MAX];         /* DB_USER, default "gradido" */
    char password[SC_DB_PASSWORD_MAX]; /* DB_PASSWORD, default empty */
    char database[SC_DB_NAME_MAX];     /* DB_DATABASE, default "gradido_community" */

    /* PostgreSQL only: how many connections a serving process holds, one per worker -- see
     * db_exec.h. */
    uint16_t pool_size; /* DB_POOL_SIZE, default SC_DB_POOL_SIZE_DEFAULT */

    /* SQLite only. Relative paths are resolved against the working directory. */
    char file[SC_DB_FILE_MAX]; /* DB_FILE, default "./gradido_community.sqlite" */

    /* 0 selects the SC_DB_CONNECT_* defaults above. They are here so a test can shorten them;
     * nothing reads them from the environment. */
    uint32_t connect_attempts;
    int64_t connect_delay_ms;
    int64_t connect_timeout_ms;
} sc_db_config;

typedef struct sc_db sc_db;

/** "postgresql" or "sqlite" -- the spelling contracts/logging.json uses for the `db` field of
 *  startup.server.started, and the one DB_TYPE is compared against. Never NULL. */
const char *sc_db_kind_name(sc_db_kind kind);

/** Non-zero when this build carries the driver for @p kind. What --version reports and what
 *  sc_db_open() checks first. */
int sc_db_kind_available(sc_db_kind kind);

/** The drivers this build has, for the startup line and for `--version`: "postgresql, sqlite",
 *  one of the two, or "none". Never NULL. */
const char *sc_db_drivers(void);

/**
 * Fills @p out from the environment, applying the documented defaults.
 *
 * Answers SC_ERR_MALFORMED for a DB_TYPE that is neither database, for a DB_PORT that is not a
 * port and for a DB_POOL_SIZE that is not a count of at least one, SC_ERR_TOO_LONG for a value
 * that would not fit, in every case having logged which variable it was.
 */
sc_status sc_db_config_load(sc_db_config *out);

/**
 * Whether @p host names a Unix socket directory rather than a host.
 *
 * A leading '/' and nothing else, which is the rule both implementations apply to the one
 * variable -- contracts/database-config.json, rules.connection. libpq needs no branch of its
 * own here, because it reads a leading '/' in `host` the same way; what needs this is the
 * password rule, which is about reaching the database over a network and not about how it is
 * spelled.
 */
int sc_db_host_is_unix_socket(const char *host);

/** Logs the effective configuration at info, once. The password is reported as present or
 *  absent and never printed. */
void sc_db_config_log(const sc_db_config *cfg);

/**
 * Opens the database @p cfg names. Allocates; this is startup.
 *
 * Answers SC_ERR_UNAVAILABLE when the build has no driver for that kind, and the driver's own
 * failure otherwise: SC_ERR_NETWORK for a database that did not answer, SC_ERR_INVALID_ARGUMENT
 * for one that answered and refused -- wrong password, no such database, no permission. The
 * distinction is what sc_db_open_waiting() needs, because waiting does not turn a wrong
 * password into a right one.
 *
 * PostgreSQL is dialled here rather than lazily, so that a database which is down at startup is
 * reported at startup. SQLite opens or creates its file.
 *
 * @p out is left untouched on failure. sc_db_error() cannot be asked what went wrong -- there
 * is no handle to ask -- so the reason is logged before this returns.
 */
sc_status sc_db_open(const sc_db_config *cfg, sc_db **out);

/**
 * Asks the database whether it is there, with the statement both drivers understand.
 *
 * Answers SC_OK, SC_ERR_NETWORK for a connection that is gone, and SC_ERR_MALFORMED for a
 * database that answered something other than a row. sc_db_error() then has the driver's own
 * sentence.
 */
sc_status sc_db_probe(sc_db *db);

/**
 * Opens the database, retrying while the failure still looks like "not yet" rather than
 * "not like this".
 *
 * A database and the server that uses it start together and the database is the slower of the
 * two. So a connection that was refused, unresolvable or timed out is tried again, up to
 * connect_attempts, and a warning line is written for each -- while an answer that says the
 * credentials are wrong or the database does not exist ends it immediately with the driver's
 * own message, because half a minute of retries would not change it.
 *
 * SQLite has nothing to wait for and is opened once whatever connect_attempts says.
 *
 * @p quit is polled while this waits, so that a Ctrl-C during half a minute of retries is
 * noticed as quickly as it is anywhere else in the process; NULL waits out the attempts. It is
 * the same flag every run loop polls -- see runtime.h.
 *
 * The final failure is returned rather than logged as fatal: whether a database that will not
 * answer stops the process is the caller's decision, and contracts/logging.json puts
 * `startup.database.failed` at the place that makes it -- not here.
 */
sc_status sc_db_open_waiting(const sc_db_config *cfg, const sc_quit_flag *quit, sc_db **out);

/** Closes the connection and gives the memory back. NULL is allowed and does nothing. */
void sc_db_close(sc_db *db);

/** Which database @p db is. */
sc_db_kind sc_db_kind_of(const sc_db *db);

/**
 * The driver's handle: a `PGconn *` for SC_DB_POSTGRESQL, an `sqlite3 *` for SC_DB_SQLITE.
 *
 * `void *` so that this header carries no driver type -- everything above service-core includes
 * it, and almost none of that has any business seeing libpq-fe.h. A repository casts it, in a
 * file that includes the driver's own header and already knows which dialect it is writing.
 * That is the same decision the file comment describes, expressed in the one line where it has
 * to be paid for.
 */
void *sc_db_native(sc_db *db);

/** What the driver said about the last failure, on one line, without a trailing newline. Never
 *  NULL; an empty string when nothing has failed. Borrowed, and valid until the next call that
 *  touches @p db. */
const char *sc_db_error(const sc_db *db);

#endif /* SERVICE_CORE_DB_H */
