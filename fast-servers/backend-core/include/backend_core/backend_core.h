/*
 * The backend domain: data, logic, interactions, repositories.
 *
 * Empty today, and the emptiness is the point -- AGENTS.md, "no feature originates in the fast
 * path". Everything that appears here is a translation of behavior that already exists in
 * packages/backend-core, arriving under the domain layout Architecture.md, *Domain structure*,
 * prescribes:
 *
 *   backend-core/src/domain/<domain>/{data,logic,interactions,repositories}/
 *
 * backend and federation both link it. dht-node does not: it discovers peers and reports them,
 * and the federation rows that follow are written by an interaction on whichever path is
 * running -- see dht-node/Architecture.md, *What this module is not*.
 */
#ifndef BACKEND_CORE_H
#define BACKEND_CORE_H

#include "service_core/config.h"
#include "service_core/status.h"

/**
 * Brings the domain up: what will be the database pool, the session cache and the repositories.
 * Called once per process, before any role starts serving, and safe to call more than once --
 * backend and federation in the same process share one domain.
 */
sc_status backend_core_init(const sc_config *cfg);

/** Counterpart of backend_core_init. The last caller tears the domain down. */
void backend_core_shutdown(void);

#endif /* BACKEND_CORE_H */
