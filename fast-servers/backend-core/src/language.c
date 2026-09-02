#include "backend_core/language.h"

#include <string.h>

/* contracts/types/Language.json, in its order. */
static const char *const kLanguages[] = {"de", "en", "es", "fr", "nl",
                                         "it", "tr", "ru", "pt", "el"};

#define BC_LANGUAGE_COUNT ((size_t)(sizeof(kLanguages) / sizeof(kLanguages[0])))

int bc_language_is_known(const char *value)
{
    size_t i;

    if (value == NULL)
        return 0;
    for (i = 0; i != BC_LANGUAGE_COUNT; ++i) {
        if (strcmp(kLanguages[i], value) == 0)
            return 1;
    }
    return 0;
}

const char *bc_language_or_default(const char *value)
{
    return bc_language_is_known(value) ? value : BC_LANGUAGE_DEFAULT;
}
