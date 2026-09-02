/*
 * The schema, as contracts/migrations defines it and this implementation runs it.
 *
 * **The migrations are not this path's property.** The TypeScript path opens the same database
 * and builds the same schema, so the SQL is a plain `.sql` file either language reads, the order
 * is a JSON file either language parses, and what is here is only what those files become after
 * they have been loaded. `contracts/migrations/index.json` is the authority on which migrations
 * exist and in what order; `contract_files.h` is how they get into the binary.
 *
 * A migration is never edited once it has run anywhere. It is followed by another one.
 *
 * What this file answers, and it answers it the same way `runMigrations.ts` does:
 *
 * ```text
 * up        bring the database to the version this build needs, at startup
 * refusal   a database whose applied migrations are not a prefix of the ones this build
 *           carries was built by other code; nothing is applied and the process stops
 * down      undo exactly one migration, never a loop of them
 * ```
 */
#ifndef BACKEND_CORE_MIGRATIONS_H
#define BACKEND_CORE_MIGRATIONS_H

#include <stddef.h>
#include <stdint.h>

#include "backend_core/database/sql.h"
#include "service_core/db.h"
#include "service_core/status.h"

/** Longest migration name the contract may carry, terminator included. */
#define BC_MIGRATION_NAME_MAX 64
/** Migrations one build may carry. A crash guard, not a plan. */
#define BC_MIGRATIONS_MAX 64
/** Longest single statement a migration may hold. One that does not fit is refused, never cut. */
#define BC_SQL_STATEMENT_MAX 8192

/** One schema step, for the dialect this process is talking to. */
typedef struct bc_migration {
    /** The number this migration is known by. Ascending, no gaps, never reused. */
    uint32_t version;
    /** What goes into `migrations.file_name`, so a row names something findable. */
    char name[BC_MIGRATION_NAME_MAX];
    const char *up;
    size_t up_len;
    /** NULL when the migration has no writable inverse -- `"down": false` in the contract.
     *  Absent is a real answer there, not a missing file: a dropped column has no inverse. */
    const char *down;
    size_t down_len;
} bc_migration;

typedef struct bc_migration_set {
    bc_migration items[BC_MIGRATIONS_MAX];
    size_t count;
} bc_migration_set;

/**
 * Reads `contracts/migrations/index.json` out of the binary and resolves every migration's SQL
 * for @p kind.
 *
 * @p error receives a sentence naming what was wrong -- a file the contract names and the build
 * did not embed, a migration with no SQL and no implementation, more migrations than this build
 * holds. Answers SC_ERR_MALFORMED for a contract this build cannot read.
 */
sc_status bc_migrations_load(sc_db_kind kind, bc_migration_set *out, char *error,
                             size_t error_size);

/** The version the code in this process expects: the highest migration it carries. */
uint32_t bc_migrations_schema_version(const bc_migration_set *set);

/**
 * Brings @p db up to the version this build needs, and answers with the version it was at
 * before through @p from_out (which may be NULL).
 *
 * Nothing is logged when there is nothing to do: a line per boot saying the schema is unchanged
 * teaches people to stop reading logs. A migration that fails is rolled back -- both databases
 * roll DDL back inside a transaction -- and `db.migration.failed` says which one.
 *
 * A schema this build cannot account for answers SC_ERR_MALFORMED, having logged
 * `db.migration.denied` in full: the migration to go back past is named there, and the caller
 * should not describe it a second time.
 */
sc_status bc_migrations_run(sc_db *db, uint32_t *from_out);

/** What `--to` is when the migration being undone is the first one: nothing is left. */
#define BC_MIGRATE_DOWN_EMPTY_TARGET "0"

/**
 * Undoes the last migration, and only the last one.
 *
 * **One step per run, as in legacy.** A down run is the operation with the worst failure mode
 * here -- `0002_users` down destroys every account -- and a loop makes the difference between "I
 * meant one" and "I meant all of them" a matter of an argument nobody checks twice. Undoing
 * three steps is running this three times, which is three decisions.
 *
 * @p target is the migration the database is to end at, one below the head, or
 * BC_MIGRATE_DOWN_EMPTY_TARGET for an empty database; NULL asks for no confirmation, which is
 * what development is allowed. It is checked before anything runs, which is the only place a
 * confirmation is worth anything -- afterwards it can only report what has already happened.
 *
 * @p error receives the reason on a refusal. Answers SC_ERR_INVALID_ARGUMENT for one, and
 * SC_ERR_MALFORMED for a schema this build cannot account for.
 */
sc_status bc_migrations_down(sc_db *db, const char *target, char *error, size_t error_size);

/* --- what a `.sql` file is made of ------------------------------------------------------- */

/**
 * Copies the next statement of @p sql into @p out, advancing @p pos.
 *
 * The rule is in `contracts/migrations/index.json` because both implementations have to split
 * the same way: a semicolon ends a statement when it ends a line, and a semicolon inside a
 * string or a dollar-quoted body does not. Without the second half this would be a split on ';'
 * that breaks the first time a migration writes `DEFAULT 'a;b'` or a PostgreSQL function body --
 * quietly, by handing the driver two halves of one statement.
 *
 * It is deliberately not a SQL parser. It knows about the two things that can contain a
 * semicolon, and about comment lines, and nothing else about the language.
 *
 * Returns 1 when @p out holds a statement, 0 when the input is used up, and -1 when a statement
 * would not fit -- refused rather than cut, which is the house rule.
 */
int bc_sql_split_next(const char *sql, size_t len, size_t *pos, char *out, size_t out_size);

#endif /* BACKEND_CORE_MIGRATIONS_H */
