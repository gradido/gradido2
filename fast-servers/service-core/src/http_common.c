/*
 * The part of the HTTP surface that does not depend on which backend was compiled in. It uses
 * only sc_http_method_is and sc_http_reply, so it links against the h2o backend and the stub
 * alike.
 */
#include "service_core/http.h"

#include <stdio.h>
#include <string.h>

const char *sc_http_reason(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        return status < 400 ? "OK" : "Error";
    }
}

int sc_http_health(sc_http_req *req, void *user_data)
{
    const char *role = (const char *)user_data;
    char body[128];
    int written;

    if (!sc_http_method_is(req, "GET"))
        return -1;

    /* Fixed buffer, explicit bounds check, 500 rather than a truncated answer -- AGENTS.md
     * section 1. A role name long enough to reach this is a configuration error, not a
     * request the client can fix by trying again. */
    written = snprintf(body, sizeof(body), "{\"status\":\"ok\",\"role\":\"%s\"}",
                       role != NULL ? role : "unknown");
    if (written <= 0 || (size_t)written >= sizeof(body)) {
        (void)sc_http_reply(req, 500, "application/json; charset=utf-8", "{}", 2);
        return 0;
    }
    (void)sc_http_reply(req, 200, "application/json; charset=utf-8", body, (size_t)written);
    return 0;
}
