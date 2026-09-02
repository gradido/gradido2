#include "service_core/api_error.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* What the contract fixes per code -- the name, the status and the template -- kept adjacent so
 * that an entry added to one and not the others is visible in the same screenful. The template is
 * here rather than at the call site for the reason the header gives. */
typedef struct sc_api_error_entry {
    sc_api_error code;
    const char *name;
    int status;
    /* `messageTemplate` in contracts/errors, as a printf format over the parameters the contract
     * names, in the order it names them. Read by exactly one function each, below. */
    const char *format;
} sc_api_error_entry;

static const sc_api_error_entry kErrors[] = {
    {SC_API_VALIDATION_FAILED, "VALIDATION_FAILED", 400, "validation failed for %s: %s"},
    {SC_API_UNKNOWN, "UNKNOWN", 500, "unknown error"},
    {SC_API_ROUTE_NOT_IMPLEMENTED, "ROUTE_NOT_IMPLEMENTED", 501,
     "route not implemented on this server: %.*s"},
};

#define SC_API_ERROR_COUNT ((size_t)(sizeof(kErrors) / sizeof(kErrors[0])))

static const sc_api_error_entry *find(sc_api_error code)
{
    size_t i;

    for (i = 0; i != SC_API_ERROR_COUNT; ++i) {
        if (kErrors[i].code == code)
            return &kErrors[i];
    }
    /* UNKNOWN, which is what a client is told about anything this build cannot name. */
    return &kErrors[1];
}

const char *sc_api_error_name(sc_api_error code)
{
    return find(code)->name;
}

int sc_api_error_status(sc_api_error code)
{
    return find(code)->status;
}

/*
 * Copies @p src into @p dst with the escaping JSON requires, and answers 0 when it would not
 * fit. Refusing rather than truncating is the house rule; log.c is the one place that goes the
 * other way, and the reason it may -- losing the event entirely -- does not apply here, because
 * the code still reaches the client.
 */
static int json_escape(char *dst, size_t dst_size, const char *src)
{
    static const char kHex[] = "0123456789abcdef";
    size_t out = 0;
    size_t i;

    for (i = 0; src[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)src[i];
        char escaped[6];
        size_t len;

        if (c == '"' || c == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)c;
            len = 2;
        } else if (c == '\n') {
            memcpy(escaped, "\\n", 2);
            len = 2;
        } else if (c == '\r') {
            memcpy(escaped, "\\r", 2);
            len = 2;
        } else if (c == '\t') {
            memcpy(escaped, "\\t", 2);
            len = 2;
        } else if (c < 0x20) {
            memcpy(escaped, "\\u00", 4);
            escaped[4] = kHex[c >> 4];
            escaped[5] = kHex[c & 0x0f];
            len = 6;
        } else {
            escaped[0] = (char)c;
            len = 1;
        }
        if (out + len >= dst_size)
            return 0;
        memcpy(dst + out, escaped, len);
        out += len;
    }
    dst[out] = '\0';
    return 1;
}

/** The one place a body is built. @p message is already formatted from the entry's template. */
static sc_status reply(sc_http_req *req, const sc_api_error_entry *entry, const char *message)
{
    char escaped[SC_API_ERROR_MESSAGE_MAX * 6 + 1];
    char body[sizeof(escaped) + 128];
    int written;

    if (req == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (!json_escape(escaped, sizeof(escaped), message))
        return SC_ERR_TOO_LONG;

    written =
        snprintf(body, sizeof(body), "{\"error\":{\"code\":%d,\"name\":\"%s\",\"message\":\"%s\"}}",
                 (int)entry->code, entry->name, escaped);
    if (written <= 0 || (size_t)written >= sizeof(body))
        return SC_ERR_TOO_LONG;

    return sc_http_reply(req, entry->status, "application/json; charset=utf-8", body,
                         (size_t)written);
}

sc_status sc_http_reply_validation_failed(sc_http_req *req, const char *field, const char *reason)
{
    const sc_api_error_entry *entry = find(SC_API_VALIDATION_FAILED);
    char message[SC_API_ERROR_MESSAGE_MAX];

    (void)snprintf(message, sizeof(message), entry->format, field != NULL ? field : "",
                   reason != NULL ? reason : "");
    return reply(req, entry, message);
}

sc_status sc_http_reply_route_not_implemented(sc_http_req *req, const char *route, size_t route_len)
{
    const sc_api_error_entry *entry = find(SC_API_ROUTE_NOT_IMPLEMENTED);
    char message[SC_API_ERROR_MESSAGE_MAX];

    /* `%.*s` and an int, because the path is not NUL terminated and may be any length. A path
     * longer than the message is cut here: the sentence is for whoever is writing a client, and
     * the code beside it is what that client decides on. */
    if (route == NULL)
        route_len = 0;
    if (route_len > (size_t)INT_MAX)
        route_len = (size_t)INT_MAX;
    (void)snprintf(message, sizeof(message), entry->format, (int)route_len,
                   route != NULL ? route : "");
    return reply(req, entry, message);
}

sc_status sc_http_reply_unknown(sc_http_req *req)
{
    const sc_api_error_entry *entry = find(SC_API_UNKNOWN);

    return reply(req, entry, entry->format);
}
