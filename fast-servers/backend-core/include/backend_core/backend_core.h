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

#include <uv.h>

#include "backend_core/domain/community.h"
#include "service_core/config.h"
#include "service_core/db.h"
#include "service_core/runtime.h"
#include "service_core/status.h"

/**
 * What an interaction serving a request is allowed to reach.
 *
 * The counterpart of packages/backend-core's `BackendContext`, minus its logger -- the log here
 * is a process-wide stream reached through service_core/log.h, so there is nothing to carry.
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
    sc_db *db;
    bc_home_community home;
    /**
     * What serialises the database, and it is an interim rather than a design.
     *
     * There is one connection and there are as many loops as the machine has cores, so without
     * this two requests write into one PGconn at the same time -- which is a data race libpq
     * documents -- and two `BEGIN ... COMMIT` sequences interleave on one SQLite handle, which
     * is a transaction containing somebody else's statements. Serialized SQLite makes each
     * *statement* safe and says nothing about a sequence of them.
     *
     * It is taken around a whole interaction rather than around a statement, because that is
     * the unit that has to be atomic: `registerAccount` looks an address up and then writes it,
     * and two of those interleaving would both find the address free and one would then fail on
     * the unique index -- a 500 that only ever happens for registered addresses, which is the
     * membership oracle the silence rule exists to prevent.
     *
     * **It is held across a database call on the request path, and that is what has to go.**
     * `Architecture.md`, *The write must be answered, not acknowledged*, has the design it is
     * standing in for: the handler defers, a thread that owns the database does the work, and the
     * loop answers when it comes back -- `sc_http_defer` and `sc_http_resume` are already there
     * for it. Until then a registration blocks the loop it arrived on for the length of one
     * write, which is affordable only because registration is rare and is not a reason to put a
     * second such lock anywhere.
     */
    uv_mutex_t db_lock;
} bc_context;

/**
 * Everything that has to be true before a request can be served, in the order it becomes true:
 * the database answers, its schema is current, and this instance knows which community it is.
 *
 * @p ask is what turns an empty database into a short conversation with whoever started the
 * process -- it fills a bc_home_community_setup and answers 1, or answers 0 when there is nobody
 * to ask. NULL is the same as answering 0. It is a parameter rather than a call into a terminal
 * from here, because asking is the role's business and not the domain's, and because a test has
 * to be able to set an instance up without one.
 *
 * Every failure is logged where it happens -- `startup.database.failed`, `db.migration.denied`,
 * `startup.setup.failed` -- so the caller decides what to do and does not describe it again.
 */
sc_status bc_context_open(const sc_db_config *db_config, const sc_quit_flag *quit,
                          int (*ask)(bc_home_community_setup *setup), bc_context *out);

/** Closes what bc_context_open opened. NULL is allowed and does nothing. */
void bc_context_close(bc_context *context);

/** Takes the database for the length of one interaction. See bc_context.db_lock. */
void bc_context_lock(bc_context *context);
void bc_context_unlock(bc_context *context);

/**
 * Brings the domain up: what will be the database pool, the session cache and the repositories.
 * Called once per process, before any role starts serving, and safe to call more than once --
 * backend and federation in the same process share one domain.
 */
sc_status backend_core_init(const sc_config *cfg);

/** Counterpart of backend_core_init. The last caller tears the domain down. */
void backend_core_shutdown(void);

#endif /* BACKEND_CORE_H */
