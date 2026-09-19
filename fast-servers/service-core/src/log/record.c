#include "record.h"

const char *const kScLogCatNames[SC_CAT__COUNT] = {
    "auth",  "user",       "transaction", "contribution", "community", "federation",
    "http",  "db",         "session",     "startup",      "mail",      "dht"};

const uint32_t kScLogGrades[SC_LOG_GRADE_COUNT] = {128u, 256u, 512u, 1024u, 2048u, 4096u};
