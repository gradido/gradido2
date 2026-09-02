/*
 * What a failing route answers with -- contracts/errors/.
 *
 * One shape for every error, so a client has one thing to parse and not one per route:
 *
 *   {"error":{"code":2003,"name":"VALIDATION_FAILED","message":"validation failed for ..."}}
 *
 * The numbers are the contract and are permanent -- contracts/AGENTS.md forbids renumbering, so
 * a value here is never reused for something else and an older frontend keeps understanding what
 * it already understood. The ranges say which file a code comes from (2000-2999 domain,
 * 3000-3999 api) and codes from both meet here, because a client does not care which of our
 * files an error was declared in. Only the codes something actually emits are listed: an enum
 * that mirrors a whole file nobody reads drifts from it silently.
 *
 * `packages/shared/src/errors` is the same list on the TypeScript path, down to the message
 * templates. The message is for a log and for whoever is writing a client; it is English and
 * never translated, and a frontend showing it to a member is showing the wrong thing -- the code
 * is what a client decides on.
 *
 * This is not a second sc_status. sc_status is transport and process outcomes inside this
 * process; these are what leaves it over HTTP, and nothing converts one into the other
 * automatically -- a route decides which contracted error a failure is, because that is a
 * business decision and not a mechanical one.
 */
#ifndef SERVICE_CORE_API_ERROR_H
#define SERVICE_CORE_API_ERROR_H

#include <stddef.h>

#include "service_core/http.h"
#include "service_core/status.h"

typedef enum sc_api_error {
    /** errors/domain.json. Replaces legacy's MutationErrorType.VALIDATION_ERROR. */
    SC_API_VALIDATION_FAILED = 2003,
    /** errors/api.json. What a client is told when the log knows more than it should. */
    SC_API_UNKNOWN = 3001,
    /** errors/api.json. This deployment's implementation does not serve that route. */
    SC_API_ROUTE_NOT_IMPLEMENTED = 3008
} sc_api_error;

/** The name the contract gives @p code, for the response body and for a log line's `err`.
 *  Never NULL; "UNKNOWN" for a code this build does not carry. */
const char *sc_api_error_name(sc_api_error code);

/** The HTTP status the contract gives @p code. 500 for one this build does not carry. */
int sc_api_error_status(sc_api_error code);

/*
 * Answering with one, and there is a call per code rather than one call taking a sentence.
 *
 * That is not a style preference, it is the shape that keeps the message contracted. A sentence
 * handed in is a sentence nothing holds to `messageTemplate`, and the contract's own history
 * shows what that costs: ROUTE_NOT_IMPLEMENTED was formatted at its one call site as
 * `route not implemented: {route}`, which is not what the contract writes, and this
 * implementation copied it from there rather than from the contract. Both were corrected
 * together; `packages/shared/src/errors` has the same shape on the TypeScript path, where
 * `errorBody` takes the template's parameters and no message at all.
 *
 * The body is built in a stack buffer; a parameter that would not fit is cut rather than
 * refused, because the code still reaches the client and the code is what a client decides on.
 */

/** `validation failed for {field}: {reason}` -- contracts/errors/domain.json. 400. */
sc_status sc_http_reply_validation_failed(sc_http_req *req, const char *field, const char *reason);

/**
 * `route not implemented on this server: {route}` -- contracts/errors/api.json. 501.
 *
 * @p route is not NUL terminated: it comes from the request line, which is where the only value
 * in any of these bodies that a client chose comes from.
 */
sc_status sc_http_reply_route_not_implemented(sc_http_req *req, const char *route,
                                              size_t route_len);

/** `unknown error` -- contracts/errors/api.json. 500, and it says nothing else on purpose: what
 *  went wrong belongs in the log and not in an answer to whoever caused it. */
sc_status sc_http_reply_unknown(sc_http_req *req);

/** Longest message this build will put into an error body. */
#define SC_API_ERROR_MESSAGE_MAX 512

#endif /* SERVICE_CORE_API_ERROR_H */
