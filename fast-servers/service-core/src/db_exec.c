/*
 * The executor: workers that own connections, a queue per cache group in front of them, and the
 * transaction rules a unit runs under. service_core/db_exec.h holds the design.
 *
 * Each group's queue is a mutex, a condition variable and an intrusive list through the units
 * themselves -- a unit lives in its request's arena, so queueing one allocates nothing. The
 * mutex is held for a pointer swap and a count, by the loops and workers of one cache group
 * and nobody else; a worker is only signalled when one is actually asleep, so under load a
 * push is a lock and a store and no system call.
 */
#include "service_core/db_exec.h"

#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "arnm/memory.h"
#include "db_internal.h"
#include "service_core/log/log.h"
#include "service_core/log/logger.h"

typedef struct exec_group {
    uv_mutex_t lock;
    uv_cond_t work;
    sc_db_unit *head;
    sc_db_unit *tail;
    uint32_t count;
    uint32_t capacity;
    uint32_t idle;
    int stopping;
    uint16_t workers;
    sc_cpu_set cpus;
} exec_group;

typedef struct exec_worker {
    sc_db_exec *exec;
    exec_group *group;
    sc_db *db;
    uv_thread_t thread;
    int started;
} exec_worker;

struct sc_db_exec {
    sc_db_exec_config cfg;
    sc_topology topology;
    sc_db_kind kind;
    uint16_t group_count;
    exec_group *groups;
    /* Which executor group each cache group's loops and workers belong to. */
    uint16_t *exec_of;
    uint16_t loop_count;
    uint16_t *loop_group;
    uint16_t worker_count;
    exec_worker *workers;
    /* SQLite: one read connection per loop, each touched by that loop's thread alone. */
    sc_db **readers;
    /* The sweeper -- see sweeper_main. */
    uv_thread_t sweeper;
    int sweeper_started;
    uv_mutex_t sweep_lock;
    uv_cond_t sweep_wake;
    int sweep_stop;
    int sweep_lock_ready;
};

/* How often queued units are checked for having waited too long. The bound a client sees is
 * SC_DB_QUEUE_WAIT_MS plus at most this. */
#define SWEEP_INTERVAL_MS 100

uint16_t sc_db_exec_workers_for(const sc_db_exec_config *cfg)
{
    if (cfg == NULL || cfg->db == NULL || cfg->db->kind == SC_DB_SQLITE)
        return 1;
    return cfg->db->pool_size != 0 ? cfg->db->pool_size : SC_DB_POOL_SIZE_DEFAULT;
}

/* --- running one unit --------------------------------------------------------------------- */

static const char *begin_text(const sc_db *db)
{
    /* IMMEDIATE on SQLite: the write lock is taken at BEGIN, not at the first write, so a unit
     * never discovers halfway through that it cannot have it. */
    return db->kind == SC_DB_SQLITE ? "BEGIN IMMEDIATE" : "BEGIN";
}

static void run_unit(sc_db *db, sc_db_unit *unit)
{
    sc_sql_error ignored;
    uint32_t reruns = 0;

    unit->status = SC_OK;
    unit->attempt = 1;
    memset(&unit->error, 0, sizeof(unit->error));

    for (;;) {
        sc_db_end end;

        db->rerun_unit = 0;
        if (unit->access == SC_DB_WRITE) {
            sc_status status = sc_sql_simple(db, begin_text(db), &unit->error);

            if (status != SC_OK) {
                unit->status = status;
                return;
            }
        }
        end = unit->work(db, unit);

        /* The session, not the unit, lost something the unit relied on -- see rerun_unit in
         * db_internal.h. Whatever the work concluded from its failed statement is thrown away
         * with the transaction, and it runs again; it is not asked, because it could not know.
         *
         * *The same attempt* again, not the next one: `attempt` is the work's own count of the
         * times it asked for AGAIN, and a unit budgets it -- a registration draws five times
         * before it calls its generator broken. A session that lost three statements must not
         * spend three of those draws. So reruns have a count of their own, and a bound of their
         * own, for a connection that loses statements faster than they can be prepared. */
        if (db->rerun_unit) {
            db->rerun_unit = 0;
            if (unit->access == SC_DB_WRITE)
                (void)sc_sql_simple(db, "ROLLBACK", &ignored);
            if (++reruns < SC_DB_RERUN_MAX)
                continue;
            sc_sql_set_error(&unit->error, SC_SQL_ERROR_OTHER,
                             "the connection kept losing its prepared statements");
            unit->status = SC_ERR_INVALID_ARGUMENT;
            return;
        }

        if (unit->access == SC_DB_WRITE) {
            if (end == SC_DB_COMMIT) {
                sc_status status = sc_sql_simple(db, "COMMIT", &unit->error);

                /* A COMMIT that fails has ended the transaction anyway, on both databases; there
                 * is nothing left to roll back. */
                if (status != SC_OK)
                    unit->status = status;
                return;
            }
            (void)sc_sql_simple(db, "ROLLBACK", &ignored);
        }
        if (end != SC_DB_AGAIN)
            return;
        if (unit->attempt >= SC_DB_AGAIN_MAX) {
            sc_sql_set_error(&unit->error, SC_SQL_ERROR_OTHER,
                             "a unit asked to run again SC_DB_AGAIN_MAX times");
            unit->status = SC_ERR_INVALID_ARGUMENT;
            return;
        }
        ++unit->attempt;
    }
}

sc_status sc_db_run(sc_db *db, sc_db_unit *unit)
{
    if (db == NULL || unit == NULL || unit->work == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    run_unit(db, unit);
    return unit->status;
}

/* --- workers ------------------------------------------------------------------------------ */

/* A connection the server closed is redialled before any work sees it -- db_postgres.c,
 * sc_db_postgres_revive, for why reading it until it is quiet is the only way to know. */
static sc_status make_usable(sc_db *db, sc_db_unit *unit)
{
    int revived = 0;
    sc_status status;

    if (db->kind != SC_DB_POSTGRESQL)
        return SC_OK;
    status = sc_db_postgres_revive(db, &revived);
    if (status != SC_OK) {
        sc_log_value data[2] = {SC_LOG_UINT("attempt", 1u), SC_LOG_UINT("attempts", 1u)};
        sc_log_context context = {0};

        context.data = data;
        context.data_count = 2;
        sc_log_event(SC_LOG_WARN, SC_CAT_DB, "db.connection.failed", &context,
                     "a worker's connection was closed by the server and could not be reopened: "
                     "%s",
                     db->error);
        sc_sql_set_error(&unit->error, SC_SQL_ERROR_CONNECTION, db->error);
    }
    return status;
}

static void run_on_worker(exec_worker *worker, sc_db_unit *unit)
{
    const uint64_t now = uv_hrtime();
    /* Read after the unit was taken, so never earlier than it was queued -- the guard is the
     * sweeper's, kept in the same form so that the two checks cannot drift apart. */
    const uint64_t waited_ns = now > unit->queued_ns ? now - unit->queued_ns : 0;
    sc_status status;

    /* Waited too long to be worth running: whoever sent it has been kept five seconds, and the
     * database that made them wait is not helped by being given the work now. */
    if (waited_ns > (uint64_t)SC_DB_QUEUE_WAIT_MS * 1000000u) {
        unit->status = SC_ERR_QUEUE_FULL;
        return;
    }
    status = make_usable(worker->db, unit);
    if (status != SC_OK) {
        unit->status = status;
        return;
    }
    run_unit(worker->db, unit);
}

static void worker_main(void *arg)
{
    exec_worker *worker = (exec_worker *)arg;
    exec_group *group = worker->group;
    sc_db_exec *exec = worker->exec;

    (void)sc_log_thread_join();
    (void)sc_topology_pin(&exec->topology, &group->cpus);

    for (;;) {
        sc_db_unit *unit;

        uv_mutex_lock(&group->lock);
        while (group->head == NULL && !group->stopping) {
            ++group->idle;
            uv_cond_wait(&group->work, &group->lock);
            --group->idle;
        }
        unit = group->head;
        if (unit == NULL) {
            /* Stopping, and nothing left: the queue drains before a worker leaves. */
            uv_mutex_unlock(&group->lock);
            break;
        }
        group->head = unit->next;
        if (group->head == NULL)
            group->tail = NULL;
        --group->count;
        uv_mutex_unlock(&group->lock);

        unit->next = NULL;
        run_on_worker(worker, unit);
        if (exec->cfg.finished != NULL)
            exec->cfg.finished(exec->cfg.context, unit);
    }
    sc_log_thread_leave();
}

/*
 * Answers the units that have waited too long, while the workers cannot.
 *
 * A worker checks a unit's age when it takes it, which is enough while the database moves. When
 * it does not -- a lock held for a minute, a server that stopped answering -- every worker is
 * stuck inside a statement and nothing is being taken, and a check at taking would leave every
 * queued request waiting for as long as the database does. This thread keeps the bound the
 * header promises: every SWEEP_INTERVAL_MS it takes the expired units off the front of each
 * queue -- the front is the oldest, so it stops at the first that is not -- and hands them back
 * as SC_ERR_QUEUE_FULL, which their callers answer 503.
 */
static void sweeper_main(void *arg)
{
    sc_db_exec *exec = (sc_db_exec *)arg;
    const uint64_t limit_ns = (uint64_t)SC_DB_QUEUE_WAIT_MS * 1000000u;

    (void)sc_log_thread_join();
    for (;;) {
        uint64_t now;
        uint16_t g;
        int stop;

        uv_mutex_lock(&exec->sweep_lock);
        if (!exec->sweep_stop)
            (void)uv_cond_timedwait(&exec->sweep_wake, &exec->sweep_lock,
                                    (uint64_t)SWEEP_INTERVAL_MS * 1000000u);
        stop = exec->sweep_stop;
        uv_mutex_unlock(&exec->sweep_lock);
        if (stop)
            break;

        for (g = 0; g != exec->group_count; ++g) {
            exec_group *group = &exec->groups[g];
            sc_db_unit *expired = NULL;
            sc_db_unit **tail = &expired;

            /* The time is read under the group's lock, not once for all groups before it: a unit
             * queued between the two would carry a queued_ns later than `now`, and `now -
             * queued_ns` on unsigned numbers is then not negative but enormous -- a request that
             * arrived a microsecond ago answered 503 as if it had waited for ever. Under the lock
             * no unit can be queued after the reading, and the comparison is written so that it
             * could not wrap even if one were. */
            uv_mutex_lock(&group->lock);
            now = uv_hrtime();
            while (group->head != NULL && now > group->head->queued_ns &&
                   now - group->head->queued_ns > limit_ns) {
                sc_db_unit *unit = group->head;

                group->head = unit->next;
                if (group->head == NULL)
                    group->tail = NULL;
                --group->count;
                unit->next = NULL;
                *tail = unit;
                tail = &unit->next;
            }
            uv_mutex_unlock(&group->lock);

            while (expired != NULL) {
                sc_db_unit *unit = expired;

                expired = unit->next;
                unit->next = NULL;
                unit->status = SC_ERR_QUEUE_FULL;
                if (exec->cfg.finished != NULL)
                    exec->cfg.finished(exec->cfg.context, unit);
            }
        }
    }
    sc_log_thread_leave();
}

/* --- groups ------------------------------------------------------------------------------- */

/*
 * The executor's groups, out of the topology's: a group per cache group that has at least one
 * worker. Workers are spread over cache groups in proportion to their CPUs, the same way loops
 * are; a cache group left without a worker -- fewer workers than L3s -- joins its nearest
 * neighbour that has one, so its loops hand work to the closest cache there is.
 */
static sc_status plan_groups(sc_db_exec *exec, uint16_t workers)
{
    const uint16_t cache_groups = exec->topology.group_count;
    uint16_t *per_cache = (uint16_t *)calloc(cache_groups, sizeof(uint16_t));
    uint16_t *exec_of = (uint16_t *)calloc(cache_groups, sizeof(uint16_t));
    uint16_t w;
    uint16_t g;
    uint16_t next = 0;

    if (per_cache == NULL || exec_of == NULL) {
        free(per_cache);
        free(exec_of);
        return SC_ERR_NO_MEMORY;
    }
    for (w = 0; w != workers; ++w)
        ++per_cache[sc_topology_group_of(&exec->topology, w, workers)];
    for (g = 0; g != cache_groups; ++g)
        exec_of[g] = per_cache[g] != 0 ? next++ : UINT16_MAX;
    exec->group_count = next;

    exec->groups = (exec_group *)calloc(exec->group_count, sizeof(exec_group));
    if (exec->groups == NULL) {
        free(per_cache);
        free(exec_of);
        return SC_ERR_NO_MEMORY;
    }
    for (g = 0; g != cache_groups; ++g) {
        uint16_t target = exec_of[g];

        if (target == UINT16_MAX) {
            /* No worker of its own: the nearest group below that has one, else above. */
            int down = (int)g - 1;
            int up = (int)g + 1;

            while (down >= 0 && exec_of[down] == UINT16_MAX)
                --down;
            while (up < cache_groups && exec_of[up] == UINT16_MAX)
                ++up;
            target = down >= 0 ? exec_of[down] : exec_of[up];
        } else {
            exec->groups[target].workers = per_cache[g];
        }
        sc_cpu_set_merge(&exec->groups[target].cpus, &exec->topology.groups[g]);
        exec_of[g] = target;
    }
    for (g = 0; g != exec->group_count; ++g)
        exec->groups[g].capacity = (uint32_t)exec->groups[g].workers * SC_DB_QUEUE_PER_WORKER;

    exec->loop_group = (uint16_t *)calloc(exec->loop_count, sizeof(uint16_t));
    if (exec->loop_group == NULL) {
        free(per_cache);
        free(exec_of);
        return SC_ERR_NO_MEMORY;
    }
    for (w = 0; w != exec->loop_count; ++w)
        exec->loop_group[w] = exec_of[sc_topology_group_of(&exec->topology, w, exec->loop_count)];

    free(per_cache);
    exec->exec_of = exec_of;
    return SC_OK;
}

/* --- open and close ----------------------------------------------------------------------- */

static void close_everything(sc_db_exec *exec)
{
    uint16_t i;

    /* The sweeper first: once the workers are told to stop they drain their queues, and a unit
     * must not be handed back twice -- once by a worker, once by the sweeper. */
    if (exec->sweeper_started) {
        uv_mutex_lock(&exec->sweep_lock);
        exec->sweep_stop = 1;
        uv_cond_signal(&exec->sweep_wake);
        uv_mutex_unlock(&exec->sweep_lock);
        (void)uv_thread_join(&exec->sweeper);
    }
    if (exec->sweep_lock_ready) {
        uv_cond_destroy(&exec->sweep_wake);
        uv_mutex_destroy(&exec->sweep_lock);
    }

    for (i = 0; i != exec->group_count; ++i) {
        uv_mutex_lock(&exec->groups[i].lock);
        exec->groups[i].stopping = 1;
        uv_cond_broadcast(&exec->groups[i].work);
        uv_mutex_unlock(&exec->groups[i].lock);
    }
    for (i = 0; i != exec->worker_count; ++i)
        if (exec->workers[i].started)
            (void)uv_thread_join(&exec->workers[i].thread);
    for (i = 0; i != exec->worker_count; ++i)
        sc_db_close(exec->workers[i].db);
    if (exec->readers != NULL)
        for (i = 0; i != exec->loop_count; ++i)
            sc_db_close(exec->readers[i]);
    for (i = 0; i != exec->group_count; ++i) {
        uv_cond_destroy(&exec->groups[i].work);
        uv_mutex_destroy(&exec->groups[i].lock);
    }
    free(exec->readers);
    free(exec->workers);
    free(exec->exec_of);
    free(exec->loop_group);
    free(exec->groups);
    free(exec);
}

sc_status sc_db_exec_open(const sc_db_exec_config *cfg, sc_db_exec **out, uint16_t *opened)
{
    sc_db_exec *exec;
    uint16_t workers;
    uint16_t made = 0;
    uint16_t i;
    sc_status status;

    if (opened != NULL)
        *opened = 0;
    if (cfg == NULL || cfg->db == NULL || out == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    exec = (sc_db_exec *)calloc(1, sizeof(*exec));
    if (exec == NULL)
        return SC_ERR_NO_MEMORY;
    exec->cfg = *cfg;
    exec->kind = cfg->db->kind;
    exec->loop_count = cfg->loops != 0 ? cfg->loops : 1;
    workers = sc_db_exec_workers_for(cfg);

    /* SQLite's one writer serves every loop and is pinned nowhere: one group, whatever the
     * machine's caches are. */
    if (cfg->topology != NULL && exec->kind == SC_DB_POSTGRESQL) {
        exec->topology = *cfg->topology;
    } else {
        exec->topology.group_count = 1;
        exec->topology.cpus_in[0] = 1;
        exec->topology.pinnable = 0;
    }

    status = plan_groups(exec, workers);
    if (status != SC_OK) {
        free(exec->groups);
        free(exec->loop_group);
        free(exec->exec_of);
        free(exec);
        return status;
    }
    for (i = 0; i != exec->group_count; ++i) {
        (void)uv_mutex_init(&exec->groups[i].lock);
        (void)uv_cond_init(&exec->groups[i].work);
    }

    exec->workers = (exec_worker *)calloc(workers, sizeof(exec_worker));
    if (exec->workers == NULL) {
        close_everything(exec);
        return SC_ERR_NO_MEMORY;
    }
    exec->worker_count = workers;

    /* The connections first, all of them, then the threads: a database that cannot hold the
     * pool stops the start before anything runs. */
    for (i = 0; i != workers; ++i) {
        exec->workers[i].exec = exec;
        exec->workers[i].group =
            &exec->groups[exec->exec_of[sc_topology_group_of(&exec->topology, i, workers)]];
        status = sc_db_open(cfg->db, &exec->workers[i].db);
        if (status != SC_OK) {
            if (opened != NULL)
                *opened = made;
            close_everything(exec);
            return status;
        }
        ++made;
    }
    if (exec->kind == SC_DB_SQLITE) {
        exec->readers = (sc_db **)calloc(exec->loop_count, sizeof(sc_db *));
        if (exec->readers == NULL) {
            close_everything(exec);
            return SC_ERR_NO_MEMORY;
        }
        for (i = 0; i != exec->loop_count; ++i) {
            status = sc_db_open(cfg->db, &exec->readers[i]);
            if (status != SC_OK) {
                if (opened != NULL)
                    *opened = made;
                close_everything(exec);
                return status;
            }
            ++made;
        }
    }
    if (opened != NULL)
        *opened = made;

    for (i = 0; i != workers; ++i) {
        if (uv_thread_create(&exec->workers[i].thread, worker_main, &exec->workers[i]) != 0) {
            close_everything(exec);
            return SC_ERR_NO_MEMORY;
        }
        exec->workers[i].started = 1;
    }
    if (uv_mutex_init(&exec->sweep_lock) != 0) {
        close_everything(exec);
        return SC_ERR_NO_MEMORY;
    }
    if (uv_cond_init(&exec->sweep_wake) != 0) {
        uv_mutex_destroy(&exec->sweep_lock);
        close_everything(exec);
        return SC_ERR_NO_MEMORY;
    }
    exec->sweep_lock_ready = 1;
    if (uv_thread_create(&exec->sweeper, sweeper_main, exec) != 0) {
        close_everything(exec);
        return SC_ERR_NO_MEMORY;
    }
    exec->sweeper_started = 1;
    *out = exec;
    return SC_OK;
}

void sc_db_exec_close(sc_db_exec *exec)
{
    if (exec != NULL)
        close_everything(exec);
}

/* --- submitting --------------------------------------------------------------------------- */

sc_status sc_db_exec_submit(sc_db_exec *exec, uint16_t loop, sc_db_unit *unit, int *ran)
{
    exec_group *group;
    sc_status status;

    if (ran != NULL)
        *ran = 0;
    if (exec == NULL || unit == NULL || unit->work == NULL || ran == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (loop >= exec->loop_count)
        loop = 0;
    unit->next = NULL;
    unit->status = SC_OK;
    unit->attempt = 0;

    /* A SQLite read on the loop's own connection, now: nothing is handed anywhere. */
    if (exec->kind == SC_DB_SQLITE && unit->access == SC_DB_READ) {
        run_unit(exec->readers[loop], unit);
        *ran = 1;
        return SC_OK;
    }

    group = &exec->groups[exec->loop_group[loop]];
    uv_mutex_lock(&group->lock);
    if (group->stopping || group->count >= group->capacity) {
        uv_mutex_unlock(&group->lock);
        return SC_ERR_QUEUE_FULL;
    }
    /* Parked before it is queued, and under the lock: once it is in the queue a worker may
     * finish it at once, and finishing a unit that was never parked would hand back something
     * nobody is waiting for. */
    if (exec->cfg.park != NULL) {
        status = exec->cfg.park(exec->cfg.context, unit);
        if (status != SC_OK) {
            uv_mutex_unlock(&group->lock);
            return status;
        }
    }
    unit->queued_ns = uv_hrtime();
    if (group->tail != NULL)
        group->tail->next = unit;
    else
        group->head = unit;
    group->tail = unit;
    ++group->count;
    if (group->idle != 0)
        uv_cond_signal(&group->work);
    uv_mutex_unlock(&group->lock);
    return SC_OK;
}

void sc_db_exec_stats_of(sc_db_exec *exec, sc_db_exec_stats *out)
{
    uint16_t i;

    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (exec == NULL)
        return;
    out->groups = exec->group_count;
    out->workers = exec->worker_count;
    for (i = 0; i != exec->group_count; ++i) {
        uv_mutex_lock(&exec->groups[i].lock);
        out->queued += exec->groups[i].count;
        uv_mutex_unlock(&exec->groups[i].lock);
    }
}

void *sc_db_unit_alloc(sc_db_unit *unit, size_t size)
{
    uint8_t *out = NULL;

    if (unit == NULL || unit->arena == NULL || size == 0 || size > UINT32_MAX)
        return NULL;
    if (arnm_alloc(&out, (uint32_t)size, (arnm *)unit->arena) != ARNM_SUCCESS)
        return NULL;
    return out;
}
