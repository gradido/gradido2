/*
 * The backend role: the HTTP server the frontend talks to.
 *
 * Routes are contracted in contracts/server/backend/. One of them is served -- user.create --
 * and every other path answers ROUTE_NOT_IMPLEMENTED rather than 404, because a deployment runs
 * one implementation and never forwards to the other.
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

/**
 * `migrate-down`: opens the database, takes it down by one migration, stops.
 *
 * **In development it runs. On a release it runs only when DB_MIGRATE_DOWN names the migration
 * one lower** -- the version the database is to end at, not the one being undone. `0` means an
 * empty database, which is what "one lower" is when the first migration is the one going away.
 *
 * A name and not a yes: it matches only when the target really is one step below where the
 * database is now, so a value left behind in an env file cannot take the next step too -- the
 * moment it has been reached it stops meaning "one lower". And it says which state was meant,
 * which for a one-step operation is the whole confirmation.
 *
 * It is a command rather than something a normal start does, and the reason is not a rule about
 * servers: a serving process migrates *up* to the version its code needs, so going down to N-1
 * and then serving a build that requires N are contradictory in one process -- it would undo the
 * step and immediately re-apply it. Going down means the next thing started is a different build.
 *
 * Deliberately not backend_run's path: that one migrates up on the way, which is the
 * contradiction above, and it asks for a home community, which a schema operation has no
 * business needing.
 */
sc_status backend_migrate_down(const sc_config *cfg, const sc_quit_flag *quit);

#endif /* BACKEND_H */
