/*
 * The federation role: the HTTP server other communities talk to.
 *
 * A separate port and a separate route set from the backend, but the same domain code -- see
 * Architecture.md. Its routes are contracted in contracts/server/federation/, versioned by API
 * version the way legacy's FEDERATION_COMMUNITY_API_PORT implies. None is served yet.
 */
#ifndef FEDERATION_H
#define FEDERATION_H

#include "service_core/config.h"
#include "service_core/runtime.h"
#include "service_core/status.h"

/** Runs until @p quit is raised. Same contract as backend_run. */
sc_status federation_run(const sc_config *cfg, const sc_quit_flag *quit);

#endif /* FEDERATION_H */
