#include "backend_core/backend_core.h"

#include "service_core/log.h"
#include "service_core/thread.h"

/*
 * Reference counted rather than a boolean: `--backend --federation` runs two roles in one
 * process against one domain, and the second one to stop is the one that tears it down.
 */
static volatile int32_t g_users;

sc_status backend_core_init(const sc_config *cfg)
{
    if (cfg == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (sc_atomic_inc(&g_users) == 1)
        sc_log_info(SC_CAT_STARTUP, "domain.init", "backend-core is up");
    /* The database pool, the session cache and the repositories are built here once they
     * exist. Nothing does yet, and inventing them ahead of the TypeScript path is what
     * AGENTS.md forbids. */
    return SC_OK;
}

void backend_core_shutdown(void)
{
    if (sc_atomic_dec(&g_users) == 0)
        sc_log_info(SC_CAT_STARTUP, "domain.shutdown", "backend-core is down");
}
