#include "backend_core/database/contract_files.h"

#include <string.h>

const bc_contract_file *bc_contract_file_find(const char *path)
{
    size_t i;

    if (path == NULL)
        return NULL;
    for (i = 0; i != bc_migration_file_count; ++i) {
        if (strcmp(bc_migration_files[i].path, path) == 0)
            return &bc_migration_files[i];
    }
    return NULL;
}
