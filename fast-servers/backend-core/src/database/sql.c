#include "backend_core/database/sql.h"

#include <stdio.h>
#include <string.h>

#include "service_core/sql.h"

void bc_sql_set_error(char *error, size_t error_size, const char *message)
{
    size_t i = 0;

    if (error == NULL || error_size == 0)
        return;
    if (message == NULL || message[0] == '\0') {
        (void)snprintf(error, error_size, "the driver gave no message");
        return;
    }
    for (; i + 1 < error_size && message[i] != '\0'; ++i)
        error[i] = (message[i] == '\n' || message[i] == '\r') ? ' ' : message[i];
    while (i > 0 && error[i - 1] == ' ')
        --i;
    error[i] = '\0';
}

sc_status bc_sql_exec(sc_db *db, const char *sql, char *error, size_t error_size)
{
    sc_sql_error failure;
    sc_status status;

    if (db == NULL || sql == NULL || error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;
    error[0] = '\0';
    status = sc_sql_simple(db, sql, &failure);
    if (status != SC_OK)
        bc_sql_set_error(error, error_size, failure.message);
    return status;
}
