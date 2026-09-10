/*
 * The backend domain: data, logic, interactions, repositories.
 *
 * Nothing here originates here -- AGENTS.md, "no feature originates in the fast path". Every line
 * of it is a translation of behavior that already exists in packages/backend-core, and it arrives
 * under the domain layout Architecture.md, *Domain structure*, prescribes:
 *
 *   backend-core/src/domain/<domain>/{data,logic,interactions,repositories}/
 *
 * backend and federation both link it. dht-node does not: it discovers peers and reports them,
 * and the federation rows that follow are written by an interaction on whichever path is
 * running -- see dht-node/Architecture.md, *What this module is not*.
 */
#ifndef BACKEND_CORE_H
#define BACKEND_CORE_H

#include "backend_core/domain/community.h"
#include "service_core/config.h"
#include "service_core/db.h"
#include "service_core/db_exec.h"
#include "service_core/http.h"
#include "service_core/runtime.h"
#include "service_core/status.h"
#include "service_core/topology.h"

/**
 * What an interaction serving a request is allowed to reach.
 *
 * The counterpart of packages/backend-core's `BackendContext`, minus its logger -- the log here
 * is a process-wide stream reached through service_core/log/log.h, so there is nothing to carry.
 *
 * It is passed down explicitly rather than being reachable from anywhere, so what a piece of code
 * touches is visible in its signature. Everything in it must be safe to lose: the database is the
 * truth, this is the working view of it, and a restart must cost nothing but a cold cache --
 * `home` included, which is read back off the one row that holds it.
 *
 * `home` is here rather than looked up per request because it is the definition of static data:
 * one row, written once at setup, changed only by an admin renaming the community. It also
 * cannot be missing -- the role refuses to start without it -- so nothing downstream has to
 * handle its absence. It carries no private key; see domain/community.h for why.
 *
 * It grows with the application: the session cache and the clients for external services belong
 * here as they are written, and an interaction that needs one will say so by reading it here.
 */
typedef struct bc_context {
    /**
     * Where database work runs -- service_core/db_exec.h. A route builds a unit in the request's
     * memory and hands it here; the executor parks the request, runs the work on a worker that
     * owns its connection, and hands the unit back on the loop the request came in on.
     *
     * It replaced a pool of connections that loops took turns holding, which in turn replaced a
     * mutex around one connection. Both kept the loop waiting while the database worked; this
     * does not, and it keeps every connection on the one thread that owns it.
     */
    sc_db_exec *exec;
    bc_home_community home;
} bc_context;

/**
 * Everything that has to be true before a request can be served, in the order it becomes true:
 * the database answers, its schema is current, and this instance knows which community it is.
 *
 * **Nothing here asks anybody anything.** A database with no community ends the start with
 * `startup.setup.failed`, reason `not-set-up`, naming the `setup` command -- see
 * backend/backend.h, backend_setup. It used to hold the conversation itself, through a callback
 * the role passed in, and moving it out is what makes a serving start something an orchestrator
 * can run: a process that may stop and wait for an answer cannot be started unattended, and one
 * that reads an answer from the terminal it logs to has to keep the two apart forever after.
 *
 * Every failure is logged where it happens -- `startup.database.failed`, `db.migration.denied`,
 * `startup.setup.failed` -- so the caller decides what to do and does not describe it again.
 *
 * Then the executor: DB_POOL_SIZE workers on PostgreSQL, the writer and a read connection per
 * loop on SQLite, grouped by @p topology the same way @p server's loops are, and joined to
 * @p server so that a unit submitted with a request comes back to that request's loop. A
 * database that will not hold every connection stops the start here, saying how many it took.
 */
sc_status bc_context_open(const sc_db_config *db_config, sc_http_server *server,
                          const sc_topology *topology, const sc_quit_flag *quit, bc_context *out);

/** Closes what bc_context_open opened. NULL is allowed and does nothing. */
void bc_context_close(bc_context *context);

/**
 * Brings the domain up: what will be the database pool, the session cache and the repositories.
 * Called once per process, before any role starts serving, and safe to call more than once --
 * backend and federation in the same process share one domain.
 */
sc_status backend_core_init(const sc_config *cfg);

/** Counterpart of backend_core_init. The last caller tears the domain down. */
void backend_core_shutdown(void);

#endif /* BACKEND_CORE_H */
