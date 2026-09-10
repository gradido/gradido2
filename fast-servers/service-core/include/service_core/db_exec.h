/*
 * Where database work runs, and who owns the connection it runs on.
 *
 * `Architecture.md`, *Databases*, *The executor*, is the design; contracts/database-config.json,
 * rules.pool, the rule both implementations follow. In short:
 *
 *   A connection belongs to one thread for its whole life -- a worker on PostgreSQL, the one
 *   writer or a loop's own reader on SQLite -- so its libpq buffers, its TLS state and its
 *   prepared statements stay in that core's cache and no lock ever guards it.
 *
 *   A request's data belongs to the loop that parsed it. What crosses between the two is a
 *   unit: the request's parameters in, its results out, once each way, and no copy -- the unit
 *   lives in the request's arena and whoever holds the unit owns the arena.
 *
 *   The loop never waits for the database. It parks the request, serves others, and is handed
 *   the unit back when the work is done.
 *
 * ### A unit
 *
 * What a caller fills in: whether it reads or writes, the function that does the work, and the
 * function that answers afterwards. `work` runs on the connection's thread with the connection;
 * `done` runs on the loop the unit came from. Neither may touch what belongs to the other --
 * `work` reads the unit's inputs and writes its outputs, and that is the whole interface.
 *
 * ### Transactions belong here
 *
 * A write unit runs inside a transaction the executor opens and ends. `work` says how it ends:
 *
 *   SC_DB_COMMIT     keep what it wrote
 *   SC_DB_ROLLBACK   keep nothing -- a registration for an address that is taken ends so
 *   SC_DB_AGAIN      keep nothing, and run `work` again in a fresh transaction, with
 *                    `attempt` one higher: a generated value that collided draws again
 *
 * No repository writes BEGIN or COMMIT. That is what lets the SQLite writer put several units
 * into one transaction later -- one fsync for many, each in its own SAVEPOINT -- without
 * touching a single unit. A read unit runs without a transaction of its own.
 *
 * ### Where a unit runs
 *
 *   PostgreSQL, read or write   a worker of the loop's cache group, queued
 *   SQLite, write               the one writer, queued
 *   SQLite, read                right here, on the loop's own read connection, before
 *                               sc_db_exec_submit returns -- a read from a warm page cache is
 *                               cheaper than handing it anywhere
 *
 * ### Too much work
 *
 * Each group's queue holds SC_DB_QUEUE_PER_WORKER units per worker, and a unit that has waited
 * SC_DB_QUEUE_WAIT_MS is not run at all -- it is handed back within a tenth of a second of that,
 * by a sweeper, even while every worker is stuck inside a statement that is not coming back.
 * Either way the unit ends with SC_ERR_QUEUE_FULL and its caller answers 503 with Retry-After:
 * the database has all the work this process may give it, and more would only make every
 * answer later.
 */
#ifndef SERVICE_CORE_DB_EXEC_H
#define SERVICE_CORE_DB_EXEC_H

#include <stddef.h>
#include <stdint.h>

#include "service_core/db.h"
#include "service_core/runtime.h"
#include "service_core/sql.h"
#include "service_core/status.h"
#include "service_core/topology.h"

/** Units one group's queue holds, per worker in the group. Past this the answer is 503. */
#define SC_DB_QUEUE_PER_WORKER 64
/**
 * How long a unit may wait in a queue before it is answered as too busy instead of run.
 *
 * Five seconds, the same bound a stopping server drains for and a contended SQLite write waits:
 * one number for how long this process lets a request stand still.
 */
#define SC_DB_QUEUE_WAIT_MS 5000
/** What a client that got 503 is told to wait, in seconds. contracts/errors/api.json,
 *  SERVICE_BUSY. */
#define SC_DB_RETRY_AFTER_S 1
/** How often a unit may ask to run again before that is taken as a bug rather than bad luck. */
#define SC_DB_AGAIN_MAX 16
/**
 * How often a unit is run again because its connection lost a prepared statement -- a
 * DISCARD ALL, a pooler -- before the connection is taken to be the problem. Each rerun
 * prepares at least the statement that was lost, so a unit gets through once it has met each
 * of its statements lost at most once; this is far above what any unit uses, and only a
 * connection that loses statements as fast as they are prepared reaches it. Counted apart from
 * SC_DB_AGAIN_MAX, and not in `attempt`: see run_unit in db_exec.c.
 */
#define SC_DB_RERUN_MAX 64

typedef enum sc_db_access { SC_DB_READ = 0, SC_DB_WRITE = 1 } sc_db_access;

typedef enum sc_db_end { SC_DB_COMMIT = 0, SC_DB_ROLLBACK, SC_DB_AGAIN } sc_db_end;

typedef struct sc_db_unit sc_db_unit;

/** The work, on the connection's thread. Answers how the transaction ends. */
typedef sc_db_end (*sc_db_work_fn)(sc_db *db, sc_db_unit *unit);

/**
 * The answer, on the loop the unit came from. @p where is what the unit was submitted with --
 * for a request, the request, or NULL when its client left while the work ran. A write that
 * committed is not undone by that; the unit is still finished.
 */
typedef void (*sc_db_done_fn)(sc_db_unit *unit, void *where);

struct sc_db_unit {
    /* The caller's. */
    sc_db_access access;
    sc_db_work_fn work;
    sc_db_done_fn done;

    /*
     * The executor's answer, readable in `done`:
     *
     *   SC_OK                 work ran, and its transaction ended the way work asked
     *   SC_ERR_QUEUE_FULL     work did not run: too much queued, or queued too long -- 503
     *   SC_ERR_NETWORK        the connection was gone and could not be redialled
     *   anything else         BEGIN or COMMIT was refused; `error` has the database's words
     *
     * What work itself decided -- an address taken, a row not found -- is the caller's own
     * business and lives in the caller's part of the unit.
     */
    sc_status status;
    /** 1 on the first run, one more for every SC_DB_AGAIN -- and only for that: a run repeated
     *  because the connection lost a prepared statement is the same attempt again. */
    uint32_t attempt;
    sc_sql_error error;

    /*
     * Where the unit lives and came from, set by whoever submits it -- the HTTP binding puts the
     * request's arena and the request here. `arena` is what sc_db_unit_alloc allocates from,
     * which is how work on a worker thread puts its results beside the request's own data
     * without a copy: while the unit is out, the arena is the unit's holder's and nobody
     * else's. `origin` is for `park` alone and means nothing after it.
     */
    void *arena;
    void *origin;

    /* The executor's own. */
    sc_db_unit *next;
    uint64_t queued_ns;
    /** For whoever parked the unit -- the HTTP binding keeps its ticket here. */
    uint64_t park_token;
};

/**
 * @p size bytes from the unit's arena, 8-byte aligned; NULL when it has none or it is full.
 * Called by whoever holds the unit -- work on its worker, done on its loop -- and by nobody else
 * at the same time, which the hand-over guarantees.
 */
void *sc_db_unit_alloc(sc_db_unit *unit, size_t size);

typedef struct sc_db_exec sc_db_exec;

typedef struct sc_db_exec_config {
    const sc_db_config *db;
    /** Threads that submit -- the server's loops. SQLite opens a read connection for each. */
    uint16_t loops;
    /** Where loops run and workers go. NULL: one group, nothing pinned. */
    const sc_topology *topology;
    /**
     * Taking a unit out of the submitter's hands before it is queued, on the submitting thread,
     * and handing it back when it is done, on the worker's. The HTTP binding parks the request
     * and resumes it (service_core/db_http.h); a test signals a condition variable. Both NULL
     * is allowed for a caller that polls.
     */
    sc_status (*park)(void *context, sc_db_unit *unit);
    void (*finished)(void *context, sc_db_unit *unit);
    void *context;
} sc_db_exec_config;

/**
 * How many workers the executor would start for @p cfg: DB_POOL_SIZE on PostgreSQL, 1 -- the
 * writer -- on SQLite.
 */
uint16_t sc_db_exec_workers_for(const sc_db_exec_config *cfg);

/**
 * Opens every connection and starts every worker.
 *
 * The database must already be reachable -- a role waits for it with sc_db_open_waiting and
 * migrates it before this -- so a connection that is refused here is the database refusing the
 * size of the pool, and the start fails with the database's own reason. @p opened, when not
 * NULL, says how many were opened before that.
 */
sc_status sc_db_exec_open(const sc_db_exec_config *cfg, sc_db_exec **out, uint16_t *opened);

/**
 * Takes @p unit, on loop @p loop's thread.
 *
 * SC_OK with @p ran set: the unit ran here and now, and its `done` has *not* been called -- the
 * caller that is still on its stack answers directly. SC_OK with @p ran clear: it is queued,
 * and `finished` will hand it back. SC_ERR_QUEUE_FULL: it was not taken.
 */
sc_status sc_db_exec_submit(sc_db_exec *exec, uint16_t loop, sc_db_unit *unit, int *ran);

/**
 * Stops the workers and closes every connection. Every unit must have been handed back, which
 * holds once the loops have stopped submitting and the queues have drained. NULL is allowed.
 */
void sc_db_exec_close(sc_db_exec *exec);

/**
 * Runs @p unit on @p db, right here, with the same transaction rules a worker applies.
 *
 * For the work that happens before there is an executor or without one: migrations' neighbours
 * at startup, the setup command, tests. Answers the unit's status. `done` is not called.
 */
sc_status sc_db_run(sc_db *db, sc_db_unit *unit);

/** What an executor is doing, for a test or a log line. */
typedef struct sc_db_exec_stats {
    uint16_t groups;
    uint16_t workers;
    uint32_t queued;
} sc_db_exec_stats;

void sc_db_exec_stats_of(sc_db_exec *exec, sc_db_exec_stats *out);

#endif /* SERVICE_CORE_DB_EXEC_H */
