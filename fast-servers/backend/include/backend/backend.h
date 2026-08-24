/*
 * The backend role: the HTTP server the frontend talks to.
 *
 * Routes are contracted in contracts/server/backend/. None of them is served yet.
 */
#ifndef BACKEND_H
#define BACKEND_H

#include "service_core/config.h"
#include "service_core/runtime.h"
#include "service_core/status.h"

/**
 * Runs until @p quit is raised, then returns. Blocking, and called on its own thread by main.
 *
 * Returns SC_OK on a clean shutdown; anything else means the role never got going, and main
 * takes the rest of the process down with it.
 */
sc_status backend_run(const sc_config *cfg, const sc_quit_flag *quit);

#endif /* BACKEND_H */
