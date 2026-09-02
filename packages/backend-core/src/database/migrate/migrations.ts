/// <reference path="../../sql.d.ts" />
// The reference is what makes `*.sql` a module for whoever compiles this file. An ambient
// declaration is only in a program that includes it, and `@gradido/backend` reaches this file
// through an import rather than through its own `include` — without the line above, the
// package typechecks here and fails there.

import communities0001DownPostgresql from '../../../../../contracts/migrations/0001_communities/down.postgresql.sql' with {
  type: 'text',
}
import communities0001DownSqlite from '../../../../../contracts/migrations/0001_communities/down.sqlite.sql' with {
  type: 'text',
}
import communities0001UpPostgresql from '../../../../../contracts/migrations/0001_communities/up.postgresql.sql' with {
  type: 'text',
}
import communities0001UpSqlite from '../../../../../contracts/migrations/0001_communities/up.sqlite.sql' with {
  type: 'text',
}
import users0002DownPostgresql from '../../../../../contracts/migrations/0002_users/down.postgresql.sql' with {
  type: 'text',
}
import users0002DownSqlite from '../../../../../contracts/migrations/0002_users/down.sqlite.sql' with {
  type: 'text',
}
import users0002UpPostgresql from '../../../../../contracts/migrations/0002_users/up.postgresql.sql' with {
  type: 'text',
}
import users0002UpSqlite from '../../../../../contracts/migrations/0002_users/up.sqlite.sql' with {
  type: 'text',
}
import index from '../../../../../contracts/migrations/index.json'
import { type DialectStatements, type Migration, splitStatements } from './Migration'

/**
 * The contract's migrations, as this implementation runs them.
 *
 * **Imported, not read.** `contracts/migrations` is the source, but a `Bun.file()` at startup
 * would only work in a checkout: `Architecture.md` requires the reference path to stay
 * independently shippable as a single binary, and a file read at runtime is not in one. A
 * text import is — `bun build --compile` embeds it.
 *
 * The cost is that the imports below are a second list beside `index.json`, and lists drift.
 * `migrations.test.ts` is what stops that: it walks the contract directory and asserts that
 * every `.sql` file there is one the contract implies and is imported here, in both
 * directions. Adding a migration is therefore: create the directory with the four files that
 * `index.json` names in `files`, add the entry, add four lines here — and if you forget the
 * last step the test says which file is missing.
 */
const SQL_FILES: Record<string, string> = {
  '0001_communities/up.postgresql.sql': communities0001UpPostgresql,
  '0001_communities/up.sqlite.sql': communities0001UpSqlite,
  '0001_communities/down.postgresql.sql': communities0001DownPostgresql,
  '0001_communities/down.sqlite.sql': communities0001DownSqlite,
  '0002_users/up.postgresql.sql': users0002UpPostgresql,
  '0002_users/up.sqlite.sql': users0002UpSqlite,
  '0002_users/down.postgresql.sql': users0002DownPostgresql,
  '0002_users/down.sqlite.sql': users0002DownSqlite,
}

/** One entry of `contracts/migrations/index.json`, before it becomes a {@link Migration}. */
type ContractedMigration = {
  readonly version: string
  readonly name: string
  readonly kind: string
  /** False when the step cannot be SQL and the contract describes it in pseudocode instead. */
  readonly sql?: boolean
  /** False when the migration has no writable inverse. `downNote` says why. */
  readonly down?: boolean
}

/**
 * The file names, read from the contract rather than repeated here.
 *
 * They are the same in every migration directory, which is why `index.json` declares them
 * once — and why this reads them from there instead of spelling them out again. A structure
 * written down in the contract and duplicated in the loader is a structure with two
 * definitions.
 */
const FILES = index.files

const contracted = index.migrations as readonly ContractedMigration[]

export const MIGRATIONS: readonly Migration[] = contracted.map(toMigration)

/** Every file the contract implies, path relative to `contracts/migrations`. */
export const MIGRATION_SQL_FILES: readonly string[] = contracted.flatMap((migration) => [
  ...(migration.sql === false ? [] : paths(migration.name, FILES.up)),
  ...(migration.sql === false || migration.down === false ? [] : paths(migration.name, FILES.down)),
])

function paths(name: string, files: { postgresql: string; sqlite: string }): string[] {
  return [`${name}/${files.postgresql}`, `${name}/${files.sqlite}`]
}

function toMigration(migration: ContractedMigration): Migration {
  if (migration.sql === false) {
    /* A data migration the contract describes in pseudocode, because it cannot be expressed
       as SQL — it needs code each implementation writes for itself. None exists yet, and the
       registry that would hold them can be added with the first one. Until then, refusing
       loudly beats starting against a database that is half migrated. */
    throw new Error(
      `migration ${migration.name} has no SQL and no implementation: see contracts/migrations`,
    )
  }

  return {
    /* Decimal string in the contract, because that is the rule there — see
       contracts/AGENTS.md, numbers are decimal strings. */
    version: Number(migration.version),
    name: migration.name,
    up: statements(migration.name, FILES.up),
    down: migration.down === false ? undefined : statements(migration.name, FILES.down),
  }
}

function statements(
  name: string,
  files: { postgresql: string; sqlite: string },
): DialectStatements {
  const read = (file: string): string[] => {
    const path = `${name}/${file}`
    const content = SQL_FILES[path]
    if (content === undefined) {
      throw new Error(`migration ${name}: ${path} is not imported in migrations.ts`)
    }
    return splitStatements(content)
  }

  return { postgresql: read(files.postgresql), sqlite: read(files.sqlite) }
}
