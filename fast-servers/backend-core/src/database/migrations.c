/*
 * See backend_core/database/migrations.h. This file is the counterpart of
 * packages/backend-core/src/database/migrate/, and the two are written against the same
 * contract rather than against each other.
 */
#include "backend_core/database/migrations.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arnm/arena.h"
#include "arnm/json_reader.h"

#include "backend_core/database/contract_files.h"
#include "service_core/log.h"

#if defined(SC_DB_WITH_SQLITE)
#include <sqlite3.h>
#endif
#if defined(SC_DB_WITH_POSTGRESQL)
#include <libpq-fe.h>
#endif

/*
 * The bookkeeping table has to exist before it can say whether anything else does, so it is the
 * one piece of DDL that is not itself a migration. `IF NOT EXISTS` rather than a version check
 * for the same reason: there is nothing to ask yet.
 */
static const char kMigrationsTablePostgresql[] = "CREATE TABLE IF NOT EXISTS migrations ("
                                                 "version integer NOT NULL PRIMARY KEY,"
                                                 "file_name varchar(256),"
                                                 "date timestamptz(3) NOT NULL DEFAULT now())";
static const char kMigrationsTableSqlite[] = "CREATE TABLE IF NOT EXISTS migrations ("
                                             "version INTEGER NOT NULL PRIMARY KEY,"
                                             "file_name TEXT,"
                                             "date INTEGER NOT NULL)";

/** A row of the `migrations` table: what this database says has been applied to it. */
typedef struct bc_applied_migration {
    uint32_t version;
    char name[BC_MIGRATION_NAME_MAX];
} bc_applied_migration;

typedef struct bc_applied_set {
    bc_applied_migration items[BC_MIGRATIONS_MAX];
    size_t count;
} bc_applied_set;

/* --- splitting a .sql file --------------------------------------------------------------- */

/** The index just past the closing quote of the string starting at @p start. */
static size_t closing_quote(const char *sql, size_t len, size_t start)
{
    size_t i = start + 1;

    while (i < len) {
        if (sql[i] == '\'') {
            /* '' inside a string is an escaped quote, not the end of one. */
            if (i + 1 < len && sql[i + 1] == '\'') {
                i += 2;
                continue;
            }
            return i + 1;
        }
        ++i;
    }
    return len;
}

/** `$$` or `$name$` at this position, or 0 if the `$` is something else. */
static size_t dollar_tag(const char *sql, size_t len, size_t start)
{
    size_t i;

    if (start + 1 < len && sql[start + 1] == '$')
        return 2;
    for (i = start + 1; i < len; ++i) {
        char c = sql[i];
        if (c == '$')
            return i == start + 1 ? 0 : i - start + 1;
        if (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            continue;
        if (i > start + 1 && c >= '0' && c <= '9')
            continue;
        return 0;
    }
    return 0;
}

/**
 * The statement without the comment lines that led up to it, trimmed.
 *
 * They are kept in the file because that is where the reasoning belongs, and dropped here
 * because a statement that is nothing but comments is not a statement -- the last "statement" of
 * every file is exactly that, everything after the final semicolon.
 */
static size_t strip_comments(char *text, size_t len)
{
    size_t read = 0;
    size_t write = 0;

    while (read < len) {
        size_t line_end = read;
        size_t start;

        while (line_end < len && text[line_end] != '\n')
            ++line_end;
        start = read;
        while (start < line_end && (text[start] == ' ' || text[start] == '\t'))
            ++start;
        if (!(start + 1 < line_end && text[start] == '-' && text[start + 1] == '-')) {
            if (write != 0)
                text[write++] = '\n';
            memmove(text + write, text + read, line_end - read);
            write += line_end - read;
        }
        read = line_end + 1;
    }
    /* trim */
    {
        size_t begin = 0;
        while (begin < write && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' ||
                                 text[begin] == '\r'))
            ++begin;
        while (write > begin && (text[write - 1] == ' ' || text[write - 1] == '\t' ||
                                 text[write - 1] == '\n' || text[write - 1] == '\r'))
            --write;
        if (begin != 0) {
            memmove(text, text + begin, write - begin);
            write -= begin;
        }
    }
    text[write] = '\0';
    return write;
}

int bc_sql_split_next(const char *sql, size_t len, size_t *pos, char *out, size_t out_size)
{
    if (sql == NULL || pos == NULL || out == NULL || out_size == 0)
        return -1;

    while (*pos <= len) {
        size_t written = 0;
        size_t i = *pos;
        int ended = 0;

        while (i < len && !ended) {
            size_t span_end = i + 1;

            if (sql[i] == ';') {
                ++i;
                ended = 1;
                break;
            }
            if (sql[i] == '\'') {
                span_end = closing_quote(sql, len, i);
            } else if (sql[i] == '$') {
                size_t tag = dollar_tag(sql, len, i);
                if (tag != 0) {
                    const char *found = NULL;
                    size_t search = i + tag;
                    while (search + tag <= len) {
                        if (memcmp(sql + search, sql + i, tag) == 0) {
                            found = sql + search;
                            break;
                        }
                        ++search;
                    }
                    /* An unterminated body is a broken migration; take the rest and let the
                     * database say so, rather than guessing where it was meant to end. */
                    span_end = found == NULL ? len : search + tag;
                }
            } else if (sql[i] == '-' && i + 1 < len && sql[i + 1] == '-') {
                const char *newline = memchr(sql + i, '\n', len - i);
                span_end = newline == NULL ? len : (size_t)(newline - sql);
            }
            if (written + (span_end - i) + 1 > out_size)
                return -1;
            memcpy(out + written, sql + i, span_end - i);
            written += span_end - i;
            i = span_end;
        }
        if (!ended)
            i = len + 1; /* the tail after the last semicolon, and then we are done */
        out[written] = '\0';
        *pos = i;
        if (strip_comments(out, written) != 0)
            return 1;
    }
    return 0;
}

/* --- the contract ------------------------------------------------------------------------ */

/* index.json is 4 KiB and its value tree a little more; this is the whole session's memory and
 * it is freed by leaving the function. Startup only -- see the file comment on the request
 * path's rule about allocation. */
#define CONTRACT_ARENA (64u * 1024u)

#define FOUND(mask, field) (((mask) & (1ull << (field))) != 0)

static int arnm_ok(arnm_result result)
{
    return result == ARNM_SUCCESS || result == ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED;
}

/** Copies a borrowed JSON string into a fixed buffer, refusing one that would not fit. */
static int block_to_buffer(const arnm_memory_block *block, char *out, size_t out_size)
{
    if (block->data == NULL || block->size + 1 > out_size)
        return 0;
    memcpy(out, block->data, block->size);
    out[block->size] = '\0';
    return 1;
}

/** The contract writes numbers as decimal strings -- contracts/AGENTS.md. 0 means unusable, and
 *  a version of 0 would be one anyway: the numbering starts at 1. */
static uint32_t block_to_version(const arnm_memory_block *block)
{
    char text[16];
    char *end = NULL;
    unsigned long parsed;

    if (block->data == NULL || block->size + 1 > sizeof(text))
        return 0;
    memcpy(text, block->data, block->size);
    text[block->size] = '\0';
    parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > 0xffffffffUL)
        return 0;
    return (uint32_t)parsed;
}

sc_status bc_migrations_load(sc_db_kind kind, bc_migration_set *out, char *error, size_t error_size)
{
    /* 8 byte aligned and a multiple of 8, as arnm_init_arena_borrow requires. On the stack
     * because this runs once per process on a role's own thread, at startup. */
    _Alignas(8) uint8_t scratch[CONTRACT_ARENA];
    arnm allocator = {0};
    arnm_json_reader reader;
    arnm_json_value *root = NULL;
    arnm_json_value *files_value = NULL;
    arnm_json_value *migrations_value = NULL;
    arnm_json_value *up_value = NULL;
    arnm_json_value *down_value = NULL;
    arnm_json_value *entries[BC_MIGRATIONS_MAX];
    const bc_contract_file *index_file;
    char up_file[64];
    char down_file[64];
    uint32_t entry_count = 0;
    uint64_t found = 0;
    uint32_t i;
    sc_status status = SC_ERR_MALFORMED;

    if (out == NULL || error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;
    out->count = 0;
    error[0] = '\0';

    index_file = bc_contract_file_find("index.json");
    if (index_file == NULL) {
        bc_sql_set_error(error, error_size, "contracts/migrations/index.json is not in this build");
        return SC_ERR_MALFORMED;
    }

    if (!arnm_ok(arnm_init_arena_borrow(&allocator, scratch, sizeof(scratch))) ||
        !arnm_ok(arnm_json_reader_init(&reader, &allocator))) {
        bc_sql_set_error(error, error_size, "no memory to read the migration contract");
        return SC_ERR_NO_MEMORY;
    }
    if (arnm_json_reader_parse(&reader, index_file->bytes, (uint32_t)index_file->length, false,
                               &root) != ARNM_SUCCESS) {
        bc_sql_set_error(error, error_size, arnm_json_reader_error_message(&reader));
        goto done;
    }

    {
        arnm_json_field fields[2] = {ARNM_JSON_FIELD_VALUE("files", &files_value),
                                     ARNM_JSON_FIELD_VALUE("migrations", &migrations_value)};
        arnm_json_read_object(root, fields, 2, &found);
        if (!FOUND(found, 0) || !FOUND(found, 1)) {
            bc_sql_set_error(error, error_size,
                             "index.json carries no files or no migrations member");
            goto done;
        }
    }

    /*
     * The file names are read from the contract rather than repeated here. They are the same in
     * every migration directory, which is why index.json declares them once -- and why this
     * reads them from there instead of spelling them out again. A structure written down in the
     * contract and duplicated in the loader is a structure with two definitions.
     */
    {
        arnm_json_field fields[2] = {ARNM_JSON_FIELD_VALUE("up", &up_value),
                                     ARNM_JSON_FIELD_VALUE("down", &down_value)};
        arnm_memory_block up_name = {0};
        arnm_memory_block down_name = {0};
        const char *dialect = sc_db_kind_name(kind);

        arnm_json_read_object(files_value, fields, 2, &found);
        if (!FOUND(found, 0) || !FOUND(found, 1)) {
            bc_sql_set_error(error, error_size, "index.json names no up or down file");
            goto done;
        }
        /* The member is named by the dialect, and the spelling is sc_db_kind_name's -- the same
         * one DB_TYPE is compared against and the same one contracts/logging.json uses. One
         * vocabulary, so a database this build can open is a database the contract can name. */
        {
            arnm_json_field pick[1] = {
                {dialect, (uint32_t)strlen(dialect), ARNM_JSON_FIELD_TYPE_STRING, &up_name}};
            arnm_json_read_object(up_value, pick, 1, &found);
            if (!FOUND(found, 0) || !block_to_buffer(&up_name, up_file, sizeof(up_file))) {
                (void)snprintf(error, error_size, "index.json names no up file for %s", dialect);
                goto done;
            }
        }
        {
            arnm_json_field pick[1] = {
                {dialect, (uint32_t)strlen(dialect), ARNM_JSON_FIELD_TYPE_STRING, &down_name}};
            arnm_json_read_object(down_value, pick, 1, &found);
            if (!FOUND(found, 0) || !block_to_buffer(&down_name, down_file, sizeof(down_file))) {
                (void)snprintf(error, error_size, "index.json names no down file for %s", dialect);
                goto done;
            }
        }
    }

    if (arnm_json_read_array(migrations_value, entries, BC_MIGRATIONS_MAX, &entry_count) !=
        ARNM_SUCCESS) {
        bc_sql_set_error(error, error_size,
                         "index.json lists more migrations than this build can hold");
        goto done;
    }

    for (i = 0; i != entry_count; ++i) {
        arnm_memory_block version = {0};
        arnm_memory_block name = {0};
        bool has_sql = true;
        bool has_down = true;
        bc_migration *migration = &out->items[i];
        char path[BC_MIGRATION_NAME_MAX + 64];
        const bc_contract_file *file;
        arnm_json_field fields[4] = {
            ARNM_JSON_FIELD_STRING("version", &version), ARNM_JSON_FIELD_STRING("name", &name),
            ARNM_JSON_FIELD_BOOL("sql", &has_sql), ARNM_JSON_FIELD_BOOL("down", &has_down)};

        arnm_json_read_object(entries[i], fields, 4, &found);
        if (!FOUND(found, 0) || !FOUND(found, 1) ||
            !block_to_buffer(&name, migration->name, sizeof(migration->name))) {
            bc_sql_set_error(error, error_size, "a migration entry has no version or no name");
            goto done;
        }
        /* Decimal string in the contract, because that is the rule there -- contracts/AGENTS.md,
         * numbers are decimal strings. */
        migration->version = block_to_version(&version);
        if (migration->version == 0) {
            (void)snprintf(error, error_size, "migration %s has no usable version",
                           migration->name);
            goto done;
        }
        if (FOUND(found, 2) && !has_sql) {
            /* A data migration the contract describes in pseudocode, because it cannot be
             * expressed as SQL -- it needs code each implementation writes for itself. None
             * exists yet, and the registry that would hold them can be added with the first one.
             * Until then, refusing loudly beats starting against a half-migrated database. */
            (void)snprintf(
                error, error_size,
                "migration %s has no SQL and no implementation: see contracts/migrations",
                migration->name);
            goto done;
        }

        (void)snprintf(path, sizeof(path), "%s/%s", migration->name, up_file);
        file = bc_contract_file_find(path);
        if (file == NULL) {
            (void)snprintf(error, error_size, "migration %s: %s is not embedded in this build",
                           migration->name, path);
            goto done;
        }
        migration->up = file->bytes;
        migration->up_len = file->length;

        if (FOUND(found, 3) && !has_down) {
            migration->down = NULL;
            migration->down_len = 0;
        } else {
            (void)snprintf(path, sizeof(path), "%s/%s", migration->name, down_file);
            file = bc_contract_file_find(path);
            if (file == NULL) {
                (void)snprintf(error, error_size, "migration %s: %s is not embedded in this build",
                               migration->name, path);
                goto done;
            }
            migration->down = file->bytes;
            migration->down_len = file->length;
        }
    }
    out->count = entry_count;
    status = SC_OK;

done:
    arnm_json_reader_release(&reader);
    return status;
}

uint32_t bc_migrations_schema_version(const bc_migration_set *set)
{
    uint32_t highest = 0;
    size_t i;

    if (set == NULL)
        return 0;
    for (i = 0; i != set->count; ++i) {
        if (set->items[i].version > highest)
            highest = set->items[i].version;
    }
    return highest;
}

/* --- what the database says has been applied --------------------------------------------- */

/*
 * All of them, not just the highest: the check below is that they are a prefix of what this
 * build carries, and a highest version alone cannot tell a database built by another branch from
 * one built by this one.
 */
static const char kSelectApplied[] =
    "SELECT version, file_name FROM migrations ORDER BY version ASC";

static sc_status applied_migrations(sc_db *db, bc_applied_set *out, char *error, size_t error_size)
{
    out->count = 0;

    switch (sc_db_kind_of(db)) {
    case SC_DB_SQLITE: {
#if defined(SC_DB_WITH_SQLITE)
        sqlite3 *handle = (sqlite3 *)sc_db_native(db);
        sqlite3_stmt *statement = NULL;
        int step;

        if (sqlite3_prepare_v2(handle, kSelectApplied, -1, &statement, NULL) != SQLITE_OK) {
            bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
            return SC_ERR_INVALID_ARGUMENT;
        }
        while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
            const unsigned char *name;

            if (out->count == BC_MIGRATIONS_MAX) {
                sqlite3_finalize(statement);
                bc_sql_set_error(error, error_size,
                                 "this database has more migrations than this build can hold");
                return SC_ERR_TOO_LONG;
            }
            out->items[out->count].version = (uint32_t)sqlite3_column_int(statement, 0);
            name = sqlite3_column_text(statement, 1);
            (void)snprintf(out->items[out->count].name, BC_MIGRATION_NAME_MAX, "%s",
                           name != NULL ? (const char *)name : "");
            ++out->count;
        }
        if (step != SQLITE_DONE) {
            bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
            sqlite3_finalize(statement);
            return SC_ERR_INVALID_ARGUMENT;
        }
        sqlite3_finalize(statement);
        return SC_OK;
#else
        bc_sql_set_error(error, error_size, "this build has no SQLite driver");
        return SC_ERR_UNAVAILABLE;
#endif
    }
    case SC_DB_POSTGRESQL:
    default: {
#if defined(SC_DB_WITH_POSTGRESQL)
        PGconn *handle = (PGconn *)sc_db_native(db);
        PGresult *result = PQexec(handle, kSelectApplied);
        int rows;
        int row;

        if (result == NULL || PQresultStatus(result) != PGRES_TUPLES_OK) {
            bc_sql_set_error(error, error_size,
                             result != NULL ? PQresultErrorMessage(result)
                                            : PQerrorMessage(handle));
            PQclear(result);
            return SC_ERR_INVALID_ARGUMENT;
        }
        rows = PQntuples(result);
        if (rows > BC_MIGRATIONS_MAX) {
            PQclear(result);
            bc_sql_set_error(error, error_size,
                             "this database has more migrations than this build can hold");
            return SC_ERR_TOO_LONG;
        }
        for (row = 0; row != rows; ++row) {
            out->items[row].version = (uint32_t)strtoul(PQgetvalue(result, row, 0), NULL, 10);
            (void)snprintf(out->items[row].name, BC_MIGRATION_NAME_MAX, "%s",
                           PQgetisnull(result, row, 1) ? "" : PQgetvalue(result, row, 1));
        }
        out->count = (size_t)rows;
        PQclear(result);
        return SC_OK;
#else
        bc_sql_set_error(error, error_size, "this build has no PostgreSQL driver");
        return SC_ERR_UNAVAILABLE;
#endif
    }
    }
}

/**
 * Where @p applied and @p carried stop telling the same story, or -1 when they do not.
 *
 * The applied migrations must be a **prefix** of the ones this build carries. Fewer is not a
 * problem -- that is work to do, and bc_migrations_run does it. More, or a different name at the
 * same version, is: it means the database was built by code this one is not, and applying
 * anything on top of it would produce a schema neither branch describes.
 *
 * Legacy refuses in both directions, because it does not migrate on startup. This keeps the
 * refusal and drops the half that is now ordinary work, and it names the migration where the two
 * part company rather than only the two ends -- that is the one a person has to migrate down
 * past.
 */
static long find_divergence(const bc_applied_set *applied, const bc_migration_set *carried)
{
    size_t i;

    for (i = 0; i != applied->count; ++i) {
        const bc_migration *known = i < carried->count ? &carried->items[i] : NULL;

        if (known != NULL && known->version == applied->items[i].version &&
            strcmp(known->name, applied->items[i].name) == 0)
            continue;
        return (long)i;
    }
    return -1;
}

/** Reports the divergence at @p position and says what to do about it. */
static void deny_schema(const bc_applied_set *applied, const bc_migration_set *carried,
                        size_t position, sc_db_kind kind)
{
    const bc_applied_migration *row = &applied->items[position];
    const bc_migration *known = position < carried->count ? &carried->items[position] : NULL;
    char target[BC_MIGRATION_NAME_MAX + 32];
    char what[256];
    sc_log_value data[4] = {SC_LOG_UINT("version", row->version), SC_LOG_STR("file", row->name),
                            SC_LOG_NULL("expected"), SC_LOG_STR("db", sc_db_kind_name(kind))};
    sc_log_context context = {0};

    if (known != NULL) {
        data[2].kind = SC_LOG_VALUE_STRING;
        data[2].text = known->name;
        (void)snprintf(what, sizeof(what),
                       "migration %u is \"%s\" in this database and \"%s\" in this build",
                       (unsigned)row->version, row->name, known->name);
    } else {
        (void)snprintf(what, sizeof(what),
                       "this database has migration %u \"%s\", which this build does not know",
                       (unsigned)row->version, row->name);
    }
    if (position == 0)
        (void)snprintf(target, sizeof(target), "an empty database");
    else
        (void)snprintf(target, sizeof(target), "%u \"%s\"",
                       (unsigned)applied->items[position - 1].version,
                       applied->items[position - 1].name);

    context.data = data;
    context.data_count = 4;
    sc_log_event(SC_LOG_FATAL, SC_CAT_DB, "db.migration.denied", &context,
                 "%s. Check out the branch the database was built with and migrate down to %s, "
                 "then start this build again -- or run a build that includes %u \"%s\"",
                 what, target, (unsigned)row->version, row->name);
}

/* --- applying one step ------------------------------------------------------------------- */

/** The `migrations` row that goes with a step, written inside the same transaction. */
static sc_status record_applied(sc_db *db, const bc_migration *migration, char *error,
                                size_t error_size)
{
    int64_t now = sc_now_ms();

    switch (sc_db_kind_of(db)) {
    case SC_DB_SQLITE: {
#if defined(SC_DB_WITH_SQLITE)
        sqlite3 *handle = (sqlite3 *)sc_db_native(db);
        sqlite3_stmt *statement = NULL;
        int step;

        if (sqlite3_prepare_v2(handle,
                               "INSERT INTO migrations (version, file_name, date) VALUES (?, ?, ?)",
                               -1, &statement, NULL) != SQLITE_OK) {
            bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
            return SC_ERR_INVALID_ARGUMENT;
        }
        sqlite3_bind_int(statement, 1, (int)migration->version);
        sqlite3_bind_text(statement, 2, migration->name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(statement, 3, now);
        step = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (step != SQLITE_DONE) {
            bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
            return SC_ERR_INVALID_ARGUMENT;
        }
        return SC_OK;
#else
        (void)now;
        bc_sql_set_error(error, error_size, "this build has no SQLite driver");
        return SC_ERR_UNAVAILABLE;
#endif
    }
    case SC_DB_POSTGRESQL:
    default: {
#if defined(SC_DB_WITH_POSTGRESQL)
        PGconn *handle = (PGconn *)sc_db_native(db);
        char version[16];
        char date[BC_TIMESTAMP_TEXT_MAX];
        const char *params[3];
        PGresult *result;
        int ok;

        (void)snprintf(version, sizeof(version), "%u", (unsigned)migration->version);
        bc_sql_timestamp_text(now, date, sizeof(date));
        params[0] = version;
        params[1] = migration->name;
        params[2] = date;
        result = PQexecParams(handle,
                              "INSERT INTO migrations (version, file_name, date) "
                              "VALUES ($1, $2, $3)",
                              3, NULL, params, NULL, NULL, 0);
        ok = result != NULL && PQresultStatus(result) == PGRES_COMMAND_OK;
        if (!ok)
            bc_sql_set_error(error, error_size,
                             result != NULL ? PQresultErrorMessage(result)
                                            : PQerrorMessage(handle));
        PQclear(result);
        return ok ? SC_OK : SC_ERR_INVALID_ARGUMENT;
#else
        (void)now;
        bc_sql_set_error(error, error_size, "this build has no PostgreSQL driver");
        return SC_ERR_UNAVAILABLE;
#endif
    }
    }
}

static sc_status forget_applied(sc_db *db, uint32_t version, char *error, size_t error_size)
{
    char statement[96];

    /* The one place a value is formatted into a statement rather than bound, and it is safe for
     * a reason that has to hold rather than be hoped for: `version` is a uint32_t out of the
     * contract, printed as digits. Nothing here comes from a request. */
    (void)snprintf(statement, sizeof(statement), "DELETE FROM migrations WHERE version = %u",
                   (unsigned)version);
    return bc_sql_exec(db, statement, error, error_size);
}

/**
 * One migration, all of it or none of it.
 *
 * Both databases roll DDL back inside a transaction, which is what makes a half-created table
 * impossible rather than merely unlikely -- and it is why the statements and the row in
 * `migrations` are written together: a schema that has changed without a row saying so is the
 * one state nothing can recover from.
 */
static sc_status apply(sc_db *db, const bc_migration *migration, const char *sql, size_t sql_len,
                       int is_down, char *error, size_t error_size)
{
    char statement[BC_SQL_STATEMENT_MAX];
    int64_t started = sc_now_ms();
    size_t pos = 0;
    sc_status status;
    int split;

    status = bc_sql_exec(db, "BEGIN", error, error_size);
    if (status != SC_OK)
        return status;

    while ((split = bc_sql_split_next(sql, sql_len, &pos, statement, sizeof(statement))) == 1) {
        status = bc_sql_exec(db, statement, error, error_size);
        if (status != SC_OK)
            goto rollback;
    }
    if (split < 0) {
        (void)snprintf(error, error_size, "a statement of migration %s is longer than %d bytes",
                       migration->name, BC_SQL_STATEMENT_MAX);
        status = SC_ERR_TOO_LONG;
        goto rollback;
    }

    status = is_down ? forget_applied(db, migration->version, error, error_size)
                     : record_applied(db, migration, error, error_size);
    if (status != SC_OK)
        goto rollback;

    status = bc_sql_exec(db, "COMMIT", error, error_size);
    if (status != SC_OK)
        goto rollback;

    if (!is_down) {
        sc_log_value data[4] = {SC_LOG_UINT("version", migration->version),
                                SC_LOG_STR("file", migration->name),
                                SC_LOG_STR("db", sc_db_kind_name(sc_db_kind_of(db))),
                                SC_LOG_UINT("ms", (uint64_t)(sc_now_ms() - started))};
        sc_log_context context = {0};

        context.data = data;
        context.data_count = 4;
        sc_log_event(SC_LOG_INFO, SC_CAT_DB, "db.migration.applied", &context,
                     "applied migration %s", migration->name);
    }
    return SC_OK;

rollback: {
    char ignored[BC_SQL_ERROR_MAX];
    (void)bc_sql_exec(db, "ROLLBACK", ignored, sizeof(ignored));
}
    if (!is_down) {
        sc_log_value data[3] = {SC_LOG_UINT("version", migration->version),
                                SC_LOG_STR("file", migration->name),
                                SC_LOG_STR("db", sc_db_kind_name(sc_db_kind_of(db)))};
        sc_log_context context = {0};

        context.data = data;
        context.data_count = 3;
        sc_log_event(SC_LOG_ERROR, SC_CAT_DB, "db.migration.failed", &context,
                     "migration %s failed and was rolled back: %s", migration->name, error);
    }
    return status;
}

/* --- up and down -------------------------------------------------------------------------- */

/** The bookkeeping table, the contract, and what the database says -- the three things both
 *  directions need before either can decide anything. */
static sc_status prepare(sc_db *db, bc_migration_set *carried, bc_applied_set *applied, char *error,
                         size_t error_size)
{
    sc_db_kind kind = sc_db_kind_of(db);
    sc_status status;

    status =
        bc_sql_exec(db, kind == SC_DB_SQLITE ? kMigrationsTableSqlite : kMigrationsTablePostgresql,
                    error, error_size);
    if (status != SC_OK)
        return status;
    status = bc_migrations_load(kind, carried, error, error_size);
    if (status != SC_OK)
        return status;
    return applied_migrations(db, applied, error, error_size);
}

sc_status bc_migrations_run(sc_db *db, uint32_t *from_out)
{
    bc_migration_set carried;
    bc_applied_set applied;
    char error[BC_SQL_ERROR_MAX];
    uint32_t from;
    long divergence;
    size_t i;
    sc_status status;

    if (db == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    status = prepare(db, &carried, &applied, error, sizeof(error));
    if (status != SC_OK) {
        sc_log_error(SC_CAT_DB, "db.query.failed", "cannot read the migration state: %s", error);
        return status;
    }

    divergence = find_divergence(&applied, &carried);
    if (divergence >= 0) {
        /* Reported here, in full, and answered with a status the caller can recognise so it does
         * not report it a second time under a heading about reaching the database. */
        deny_schema(&applied, &carried, (size_t)divergence, sc_db_kind_of(db));
        return SC_ERR_MALFORMED;
    }

    from = applied.count == 0 ? 0 : applied.items[applied.count - 1].version;
    if (from_out != NULL)
        *from_out = from;

    /* Nothing is logged when there is nothing to do. The startup line already says which
     * database this is, and a line per boot saying that the schema is unchanged is the kind of
     * noise that teaches people to stop reading logs. */
    for (i = 0; i != carried.count; ++i) {
        const bc_migration *migration = &carried.items[i];

        if (migration->version <= from)
            continue;
        status = apply(db, migration, migration->up, migration->up_len, 0, error, sizeof(error));
        if (status != SC_OK)
            return status;
    }
    return SC_OK;
}

sc_status bc_migrations_down(sc_db *db, const char *target, char *error, size_t error_size)
{
    bc_migration_set carried;
    bc_applied_set applied;
    const bc_migration *head;
    const char *reached;
    long divergence;
    sc_status status;

    if (db == NULL || error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;

    status = prepare(db, &carried, &applied, error, error_size);
    if (status != SC_OK)
        return status;

    divergence = find_divergence(&applied, &carried);
    if (divergence >= 0) {
        /* The same refusal as going up, and for a stronger reason: undoing a migration this
         * build does not have would run the wrong SQL against the right table. */
        deny_schema(&applied, &carried, (size_t)divergence, sc_db_kind_of(db));
        (void)snprintf(error, error_size,
                       "this database was built by other code; see db.migration.denied");
        return SC_ERR_MALFORMED;
    }

    if (applied.count == 0) {
        (void)snprintf(error, error_size,
                       "nothing to undo: this database has no migrations applied");
        return SC_ERR_INVALID_ARGUMENT;
    }
    head = &carried.items[applied.count - 1];

    if (target != NULL) {
        /* One below the head is what a down step reaches. "0" is that when the head is the first
         * migration -- there is no migration named for an empty database. */
        reached = applied.count >= 2 ? carried.items[applied.count - 2].name
                                     : BC_MIGRATE_DOWN_EMPTY_TARGET;
        if (strcmp(target, reached) != 0) {
            (void)snprintf(error, error_size,
                           "this database is at \"%s\", so one migration lower is \"%s\" -- the "
                           "confirmation names \"%s\". Nothing was undone.",
                           head->name, reached, target);
            return SC_ERR_INVALID_ARGUMENT;
        }
    }
    if (head->down == NULL) {
        (void)snprintf(error, error_size,
                       "migration %u \"%s\" has no down step and cannot be undone: see "
                       "contracts/migrations",
                       (unsigned)head->version, head->name);
        return SC_ERR_INVALID_ARGUMENT;
    }

    status = apply(db, head, head->down, head->down_len, 1, error, error_size);
    if (status != SC_OK)
        return status;

    {
        sc_log_value data[3] = {SC_LOG_UINT("version", head->version),
                                SC_LOG_STR("file", head->name),
                                SC_LOG_STR("db", sc_db_kind_name(sc_db_kind_of(db)))};
        sc_log_context context = {0};

        context.data = data;
        context.data_count = 3;
        sc_log_event(SC_LOG_WARN, SC_CAT_DB, "db.migration.reverted", &context,
                     "undid migration %s; the database is now at version %u", head->name,
                     (unsigned)head->version - 1);
    }
    return SC_OK;
}
