/*
 * Status codes shared by every service-core call.
 *
 * These are transport-level and process-level outcomes only. Business errors are contracted
 * in contracts/errors/ and carry their own numbering; nothing here is allowed to grow into a
 * second, competing error taxonomy.
 */
#ifndef SERVICE_CORE_STATUS_H
#define SERVICE_CORE_STATUS_H

typedef enum sc_status {
    SC_OK = 0,
    /* the caller passed something the function cannot work with */
    SC_ERR_INVALID_ARGUMENT = -1,
    /* a fixed-size buffer would have had to truncate; see AGENTS.md, house dialect */
    SC_ERR_TOO_LONG = -2,
    /* out of memory, at startup -- the request path does not allocate */
    SC_ERR_NO_MEMORY = -3,
    /* socket, bind, listen, accept */
    SC_ERR_NETWORK = -4,
    /* the surface exists, the implementation does not yet */
    SC_ERR_NOT_IMPLEMENTED = -5,
    /* compiled out: the build selected a backend that cannot do this */
    SC_ERR_UNAVAILABLE = -6,
    /* the value was there but is not what its type says it is */
    SC_ERR_MALFORMED = -7,
    /* a bounded queue is at its limit. Not an error the callee can resolve: only the caller
     * knows whether the work behind the entry may be dropped or has to stop. */
    SC_ERR_QUEUE_FULL = -8
} sc_status;

/** Human-readable name of @p status, for a log line. Never NULL. */
const char *sc_status_name(sc_status status);

#endif /* SERVICE_CORE_STATUS_H */
