/*
 * Who may call this server from a browser -- the counterpart of `corsPlugin()` in
 * packages/backend/src/index.ts.
 *
 * A Gradido deployment serves the frontend and the backend from one origin -- the frontend is a
 * small mithril bundle, and hosting it apart from the server it talks to buys nothing -- so in
 * production the browser has no cross-origin question to ask and these headers are never read.
 * Everything except loopback is therefore refused there rather than wildcarded:
 * `Access-Control-Allow-Origin: *` could not carry a session cookie anyway, but it would let any
 * page on the internet read every unauthenticated answer, and that is surface nobody asked for.
 *
 * **Loopback is answered in every mode**, because a page on this machine is a case that keeps
 * turning up whatever NODE_ENV says: the vite dev server on its own port, a local admin page,
 * something someone is debugging against a production build. Only a page actually served from
 * the loopback interface can present one of these as its `Origin` -- a site on the internet
 * sends its own origin, and nothing it can do to DNS changes that -- so answering them widens
 * nothing that faces outward.
 *
 * In development anything is answered, so a phone on the same network can reach the dev server.
 * The origin is echoed rather than wildcarded in both cases: the session cookie makes every call
 * credentialed, and a browser refuses `*` for those.
 *
 * ### Where this deliberately does not copy the reference path
 *
 * The reference mounts `@elysiajs/cors`, and that plugin puts three things on the wire that the
 * configuration above does not ask for. They are not reproduced here, and `AGENTS.md` section 0
 * is why -- "do not force artificial parity, preserve the business semantics". Each was checked
 * against what a browser does with it before it was left out:
 *
 *   Access-Control-Allow-Headers on a response that is not a preflight. A browser reads that
 *     header only on a preflight; on anything else it is ignored. The plugin fills it with the
 *     names of the headers the *request* carried.
 *   Access-Control-Expose-Headers, filled with those same request header names. Expose-Headers
 *     names response headers a script may read, so a list of request header names exposes
 *     nothing that exists. The frontend reads no response header at all.
 *   Access-Control-Allow-Headers: undefined, which is what the plugin sends on a preflight that
 *     carried no Access-Control-Request-Headers. A browser that asked about no headers checks
 *     none, so the literal string is inert -- it is a defect rather than a behaviour.
 *
 * Everything a browser acts on is the same on both paths, and the integration between them was
 * compared request by request rather than reasoned about.
 */
#include "cors.h"

#include <stdio.h>
#include <string.h>

#include "service_core/http.h"

/* The four the reference configures. A method absent from this list is refused by the browser
 * before the request is made, which is the only place it can be refused cheaply. */
static const char kAllowMethods[] = "GET, POST, PUT, DELETE";

/*
 * Five seconds, which is what the reference path sends and also what a browser assumes when no
 * maximum is given. Short on purpose: a preflight is one round trip against a policy that a
 * deployment may change, and this route set is not one a page walks through in a loop.
 */
static const char kMaxAge[] = "5";

/**
 * A page served from this machine, whatever port it is on.
 *
 * `localhost`, `127.0.0.1` and `[::1]`, http or https -- the same set as LOOPBACK_ORIGIN in
 * packages/backend/src/index.ts, written out because a regular expression here would be a
 * dependency for one predicate.
 */
static int is_loopback_origin(const char *origin, size_t len)
{
    static const char *const kHosts[] = {"localhost", "127.0.0.1", "[::1]"};
    size_t offset = 0;
    size_t i;

    if (len > 8 && memcmp(origin, "https://", 8) == 0)
        offset = 8;
    else if (len > 7 && memcmp(origin, "http://", 7) == 0)
        offset = 7;
    else
        return 0;

    for (i = 0; i != sizeof(kHosts) / sizeof(kHosts[0]); ++i) {
        size_t host_len = strlen(kHosts[i]);
        size_t rest;

        if (len - offset < host_len || memcmp(origin + offset, kHosts[i], host_len) != 0)
            continue;
        rest = len - offset - host_len;
        if (rest == 0)
            return 1;
        /* `:` and then digits, and nothing else: a port, not a path and not another host that
         * merely starts the same way. */
        if (origin[offset + host_len] != ':' || rest < 2)
            return 0;
        {
            size_t d;
            for (d = offset + host_len + 1; d != len; ++d) {
                if (origin[d] < '0' || origin[d] > '9')
                    return 0;
            }
        }
        return 1;
    }
    return 0;
}

int backend_cors(sc_http_req *req, void *user_data)
{
    const backend_cors_policy *policy = (const backend_cors_policy *)user_data;
    size_t origin_len = 0;
    const char *origin = sc_http_header(req, "origin", &origin_len);
    int preflight = sc_http_method_is(req, "OPTIONS");

    /* Credentials always, because the session travels in a cookie and a browser sends one
     * cross-origin only when it is told to. It is not a permission on its own: without an
     * Allow-Origin beside it, the browser refuses the response anyway. */
    (void)sc_http_header_add(req, "access-control-allow-credentials", "true", 4);
    /* `*` in development says the answer depends on so much that it may not be cached at all;
     * in production the answer varies by exactly one header, and saying which lets a cache in
     * front of this server keep working. Both are what the reference sends. */
    if (policy->development)
        (void)sc_http_header_add(req, "vary", "*", 1);
    else
        (void)sc_http_header_add(req, "vary", "origin", 6);

    if (origin != NULL && (policy->development || is_loopback_origin(origin, origin_len))) {
        (void)sc_http_header_add(req, "access-control-allow-origin", origin, origin_len);
    } else if (origin == NULL && policy->development) {
        /* No Origin is not a cross-origin request and no browser will read this. The reference
         * sends `*` here and it costs nothing to say the same. */
        (void)sc_http_header_add(req, "access-control-allow-origin", "*", 1);
    }
    /* An origin that is not allowed gets no Allow-Origin at all, which is how a browser is told
     * no: there is no header that says "refused", only the absence of one that says yes. */

    (void)sc_http_header_add(req, "access-control-allow-methods", kAllowMethods,
                             sizeof(kAllowMethods) - 1);

    if (!preflight)
        return -1; /* the route answers; the headers above ride along on whatever it writes */

    {
        size_t requested_len = 0;
        const char *requested =
            sc_http_header(req, "access-control-request-headers", &requested_len);

        /* Echoed rather than listed, which is what the reference does and what makes a route
         * usable by a client sending a header this server has never heard of. The refusal that
         * matters is the origin, above. */
        if (requested != NULL)
            (void)sc_http_header_add(req, "access-control-allow-headers", requested, requested_len);
        (void)sc_http_header_add(req, "access-control-max-age", kMaxAge, sizeof(kMaxAge) - 1);
    }

    /* A preflight is answered here and never reaches a route: it asks about a request that has
     * not been made, so there is nothing for a handler to do with it -- and answering it here is
     * what lets a path this server does not serve still be preflighted, exactly as the reference
     * does through its plugin. */
    (void)sc_http_reply(req, 204, "", "", 0);
    return 0;
}
