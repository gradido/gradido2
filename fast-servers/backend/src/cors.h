/*
 * The CORS policy, as the role configures it. See cors.c for what it is and why.
 */
#ifndef BACKEND_CORS_H
#define BACKEND_CORS_H

#include "service_core/http.h"

typedef struct backend_cors_policy {
    /**
     * NODE_ENV=development, which is the default on both paths.
     *
     * Development answers any origin, so a phone on the same network reaches the dev server;
     * production answers loopback and nothing else. It is the only thing this policy varies on.
     */
    int development;
} backend_cors_policy;

/** Registered with sc_http_before_route, so that it runs for every request and not per route. */
int backend_cors(sc_http_req *req, void *user_data);

#endif /* BACKEND_CORS_H */
