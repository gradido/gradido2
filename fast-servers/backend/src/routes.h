/*
 * The handlers backend.c registers, one declaration per file that holds one.
 *
 * Not a handler interface: these are ordinary functions of the shape service_core/http.h already
 * defines, and the header exists so that backend.c can name them without a header per route. The
 * reference path made the same decision in reverse -- see packages/backend/src/server/, which
 * removed the handler interface it used to have.
 */
#ifndef BACKEND_ROUTES_H
#define BACKEND_ROUTES_H

#include "service_core/http.h"

/** `POST /user/create` -- contracts/server/backend/user.json, `user.create`. @p user_data is the
 *  bc_context. */
int backend_user_create(sc_http_req *req, void *user_data);

/**
 * Every path no route matched.
 *
 * Of the 139 routes in contracts/server, nearly all are still unwritten, so an unknown path on
 * this server is overwhelmingly a contracted route rather than a typo -- and the contract
 * requires that case to be answered rather than 404'd, because a deployment never forwards to
 * the other implementation. A genuine typo gets the same answer, which is the price of not
 * loading the contract at runtime.
 */
int backend_route_not_implemented(sc_http_req *req, void *user_data);

/**
 * What the server registers as its default route: the pages first, the answer above second.
 *
 * The order is the whole design. A static server consulted *before* the routes would shadow
 * them; one that answered a 404 itself would take ROUTE_NOT_IMPLEMENTED away from every
 * contracted path that is not written yet. So it runs last and only claims what it recognises
 * -- see backend/static_sites.h. The reference path mounts the same two in the same order,
 * with the static plugin last and its NotFoundError falling through to the error handler.
 *
 * It lives in static_sites.c, with the half of it that can decline.
 */
int backend_route_default(sc_http_req *req, void *user_data);

#endif /* BACKEND_ROUTES_H */
