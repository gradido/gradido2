/*
 * SQLite, in this process.
 *
 * It is a C library called directly, and Architecture.md, *Databases*, settles it the same way
 * it settles libpq: a binding in any language emits the same call to the same sqlite3_*
 * function, so there is nothing here for a different client to be faster at. What this file
 * decides is not the driver but the three settings a connection is opened with, and each of
 * them is a correctness decision rather than a preference:
 *
 *   journal_mode = WAL   readers and one writer at the same time. Without it a role thread
 *                        reading blocks the one writing, which is the whole difference between
 *                        SQLite as a small community's database and SQLite as a toy. The
 *                        TypeScript path sets it on the same connection, for the same reason.
 *   foreign_keys = ON    off by default in SQLite, and per connection rather than per database.
 *                        A build that forgets it enforces no constraint the schema declares.
 *   busy_timeout         several role threads share this process. Without it a write that meets
 *                        another write answers SQLITE_BUSY immediately rather than waiting the
 *                        moment out.
 *
 * The compile-time options the amalgamation is built with are in build.zig, beside the reason
 * for each. SQLITE_DQS=0 is the one worth knowing from here: a double-quoted string is a string
 * and never an identifier that fell back to one, so a misspelled column name is an error rather
 * than a silently constant value.
 */
#include "service_core/db.h"

#include "db_internal.h"

#if defined(SC_DB_WITH_SQLITE)

#include <string.h>

#include <sqlite3.h>

#include "service_core/log.h"

/*
 * How long a statement waits for another thread's write before it gives up.
 *
 * Five seconds is long enough that a contended commit waits rather than fails, and short enough
 * that a deadlock is reported instead of looking like a hang. It is not configurable yet
 * because nothing has measured it; when something does, it belongs in sc_db_config beside the
 * connect timeouts.
 */
#define SQLITE_BUSY_TIMEOUT_MS 5000

int sc_db_sqlite_available(void)
{
    return 1;
}

/** True for the in-memory databases a test opens, which have no file and cannot keep a WAL. */
static int is_memory_database(const char *file)
{
    return strcmp(file, ":memory:") == 0 || strncmp(file, "file::memory:", 13) == 0;
}

static sc_status exec_simple(sc_db *db, const char *sql)
{
    sqlite3 *handle = (sqlite3 *)db->native;
    char *message = NULL;

    if (sqlite3_exec(handle, sql, NULL, NULL, &message) != SQLITE_OK) {
        sc_db_set_error(db, message != NULL ? message : sqlite3_errmsg(handle));
        sqlite3_free(message);
        return SC_ERR_INVALID_ARGUMENT;
    }
    sqlite3_free(message);
    return SC_OK;
}

/**
 * Switches the database to WAL and reports what it actually got.
 *
 * `PRAGMA journal_mode` answers with the mode it ended up in rather than with an error, and it
 * does refuse: a database on a network filesystem, and an in-memory one, stay where they are.
 * Silently continuing in `delete` mode would look like a working server that serialises every
 * reader against the writer, which is the failure that shows up as "SQLite is slow" months
 * later. So the answer is read and a mode that is not WAL is said out loud.
 */
static sc_status set_wal(sc_db *db, const sc_db_config *cfg)
{
    sqlite3 *handle = (sqlite3 *)db->native;
    sqlite3_stmt *stmt = NULL;
    const char *mode = NULL;

    if (sqlite3_prepare_v2(handle, "PRAGMA journal_mode = WAL", -1, &stmt, NULL) != SQLITE_OK) {
        sc_db_set_error(db, sqlite3_errmsg(handle));
        return SC_ERR_INVALID_ARGUMENT;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW)
        mode = (const char *)sqlite3_column_text(stmt, 0);
    if (mode != NULL && sqlite3_stricmp(mode, "wal") != 0 && !is_memory_database(cfg->file)) {
        sc_log_warn(SC_CAT_DB, "db.connection.failed",
                    "sqlite refused WAL and stayed in %s mode for %s -- readers and the writer "
                    "will block each other",
                    mode, cfg->file);
    }
    sqlite3_finalize(stmt);
    return SC_OK;
}

sc_status sc_db_sqlite_open(const sc_db_config *cfg, sc_db *db)
{
    sqlite3 *handle = NULL;
    sc_status status;
    int rc;

    /* FULLMUTEX because the roles are threads and one connection may be reached from more than
     * one of them. SQLITE_OPEN_CREATE because a community's first start has no file yet, which
     * is what "download and start" means. */
    rc = sqlite3_open_v2(cfg->file, &handle,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        /* sqlite3_open_v2 hands back a handle even when it failed, and it is the only thing
         * that knows why. */
        sc_db_set_error(db, handle != NULL ? sqlite3_errmsg(handle) : sqlite3_errstr(rc));
        sqlite3_close_v2(handle);
        /* Not SC_ERR_NETWORK: there is no second party here, so nothing about this gets better
         * by being asked again. A broken or unreadable file will still be one in a second. */
        return SC_ERR_INVALID_ARGUMENT;
    }
    db->native = handle;

    (void)sqlite3_busy_timeout(handle, SQLITE_BUSY_TIMEOUT_MS);

    status = set_wal(db, cfg);
    if (status == SC_OK)
        status = exec_simple(db, "PRAGMA foreign_keys = ON");
    if (status != SC_OK) {
        sqlite3_close_v2(handle);
        db->native = NULL;
        return status;
    }
    return SC_OK;
}

sc_status sc_db_sqlite_probe(sc_db *db)
{
    sqlite3 *handle = (sqlite3 *)db->native;
    sqlite3_stmt *stmt = NULL;
    sc_status status = SC_OK;

    if (handle == NULL) {
        sc_db_set_error(db, "no connection");
        return SC_ERR_INVALID_ARGUMENT;
    }
    if (sqlite3_prepare_v2(handle, "select 1", -1, &stmt, NULL) != SQLITE_OK) {
        sc_db_set_error(db, sqlite3_errmsg(handle));
        return SC_ERR_MALFORMED;
    }
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sc_db_set_error(db, sqlite3_errmsg(handle));
        status = SC_ERR_MALFORMED;
    }
    sqlite3_finalize(stmt);
    return status;
}

void sc_db_sqlite_close(sc_db *db)
{
    /* close_v2 rather than close: it lets go of a handle that still has statements on it
     * instead of answering SQLITE_BUSY and leaking the connection. */
    (void)sqlite3_close_v2((sqlite3 *)db->native);
    db->native = NULL;
}

#else /* the build was told to leave this driver out */

/* NULL, and nothing else -- the driver's own header is what carried it in the branch above. */
#include <stddef.h>

int sc_db_sqlite_available(void)
{
    return 0;
}

/* See db_postgres.c for why these exist rather than an #if around the dispatch. */
sc_status sc_db_sqlite_open(const sc_db_config *cfg, sc_db *db)
{
    (void)cfg;
    sc_db_set_error(db, "this build has no SQLite driver; it was built with -Dsqlite=false");
    return SC_ERR_UNAVAILABLE;
}

sc_status sc_db_sqlite_probe(sc_db *db)
{
    sc_db_set_error(db, "this build has no SQLite driver");
    return SC_ERR_UNAVAILABLE;
}

void sc_db_sqlite_close(sc_db *db)
{
    db->native = NULL;
}

#endif /* SC_DB_WITH_SQLITE */
