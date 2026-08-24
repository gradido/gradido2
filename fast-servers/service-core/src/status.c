#include "service_core/status.h"

const char *sc_status_name(sc_status status)
{
    switch (status) {
    case SC_OK:
        return "ok";
    case SC_ERR_INVALID_ARGUMENT:
        return "invalid_argument";
    case SC_ERR_TOO_LONG:
        return "too_long";
    case SC_ERR_NO_MEMORY:
        return "no_memory";
    case SC_ERR_NETWORK:
        return "network";
    case SC_ERR_NOT_IMPLEMENTED:
        return "not_implemented";
    case SC_ERR_UNAVAILABLE:
        return "unavailable";
    case SC_ERR_MALFORMED:
        return "malformed";
    }
    return "unknown";
}
