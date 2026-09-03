/*
 * The static web server: the pages, out of the same process that serves the routes they call.
 *
 * The rules are `packages/backend/src/server/staticRoutes.ts`'s, because a client must not be
 * able to tell which implementation answered. The one that is worth stating twice is what
 * happens to a path no file matches:
 *
 *   a browser navigating to /login   gets the app -- it is a route of the mithril app, and a
 *                                    bookmark to one has to work
 *   anything that did not ask for    is handed back, and becomes the contracted
 *   text/html                        ROUTE_NOT_IMPLEMENTED
 *
 * Nearly every path in contracts/server is still unwritten, so answering all of them with an
 * HTML page would hide 139 routes behind a login screen -- and a client that parsed the app as
 * its answer would report something that has nothing to do with what went wrong. The Accept
 * header is what tells a browser from a client, and nothing more clever is needed.
 *
 * Which file a path means is `static_sites.c`; this is what is done with the answer.
 */
#include "backend/static_sites.h"

#include <string.h>

#include "routes.h"
#include "service_core/http.h"

#define CACHE_IMMUTABLE "public, max-age=31536000, immutable"

/* Cache it, but ask every time whether it is still current -- the ETag answers cheaply. */
#define CACHE_REVALIDATE "no-cache"

/** Whether @p req asked for a copy it already has, and got no newer one. */
static int is_unchanged(sc_http_req *req, const backend_static_file *file)
{
    size_t length = 0;
    const char *value = sc_http_header(req, "if-none-match", &length);

    /* One exact tag, the way the TypeScript server compares it: a list of them and the weak
     * form are what a proxy sends, and neither is what a browser revalidating a page it was
     * given sends back. */
    return value != NULL && length == strlen(file->etag) && memcmp(value, file->etag, length) == 0;
}

/** Whether the caller wants a page rather than an answer. */
static int wants_html(sc_http_req *req)
{
    size_t length = 0;
    const char *accept = sc_http_header(req, "accept", &length);
    const size_t needle = sizeof("text/html") - 1;
    size_t i;

    if (accept == NULL || length < needle)
        return 0;
    for (i = 0; i + needle <= length; ++i) {
        if (memcmp(accept + i, "text/html", needle) == 0)
            return 1;
    }
    return 0;
}

static int send_file(sc_http_req *req, const backend_static_file *file)
{
    const char *cache = file->immutable ? CACHE_IMMUTABLE : CACHE_REVALIDATE;

    (void)sc_http_header_add(req, "cache-control", cache, strlen(cache));
    (void)sc_http_header_add(req, "etag", file->etag, strlen(file->etag));

    if (is_unchanged(req, file)) {
        /* No body and nothing describing one -- sc_http_reply drops both for this status, on
         * either backend. The headers above still go out, which is what a revalidating cache
         * came for. */
        (void)sc_http_reply(req, 304, "", "", 0);
        return 0;
    }
    /* The bytes are in the executable, so they are handed to the socket rather than copied:
     * that is the whole of sc_http_reply_static, and it is also what lets the fallback backend
     * answer with a file larger than the buffer it serialises a response into. */
    (void)sc_http_reply_static(req, 200, file->content_type, file->bytes, file->length);
    return 0;
}

int backend_static_serve(sc_http_req *req, void *user_data)
{
    const backend_static_site *site;
    const backend_static_file *file;
    const char *path;
    size_t path_length = 0;
    size_t relative_length;
    const char *relative;

    (void)user_data;

    /* A page is fetched, never posted. Anything else belongs to a route, and there is no
     * method that makes a file into one. */
    if (!sc_http_method_is(req, "GET"))
        return -1;

    path = sc_http_path(req, &path_length);
    if (path == NULL)
        return -1;

    site =
        backend_static_site_for(backend_static_sites, backend_static_site_count, path, path_length);
    if (site == NULL)
        return -1;

    relative = path + site->base_path_length;
    relative_length = path_length - site->base_path_length;
    while (relative_length != 0 && relative[0] == '/') {
        ++relative;
        --relative_length;
    }

    /* The site's own root is the app, whoever is asking. Not the Accept rule below: a bare "/"
     * is a person opening the server, not a client that took a wrong turn. */
    if (relative_length == 0)
        return send_file(req, site->index);

    file = backend_static_file_find(site, relative, relative_length);
    if (file != NULL)
        return send_file(req, file);

    if (!wants_html(req))
        return -1;
    return send_file(req, site->index);
}

int backend_route_default(sc_http_req *req, void *user_data)
{
    /* The pages are given first refusal and take only what they recognise -- a file they have,
     * or a browser navigating to a route of the app. Everything else is a contracted route
     * this implementation does not serve yet, and says so. */
    if (backend_static_serve(req, NULL) == 0)
        return 0;
    return backend_route_not_implemented(req, user_data);
}
