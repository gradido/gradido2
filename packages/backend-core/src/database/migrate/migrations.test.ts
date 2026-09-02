import { describe, expect, test } from 'bun:test'
import { readdirSync } from 'node:fs'
import { join, sep } from 'node:path'
import { findSchemaDivergence, schemaDivergenceMessage, splitStatements } from './Migration'
import { MIGRATION_SQL_FILES, MIGRATIONS } from './migrations'

const CONTRACT_DIRECTORY = join(import.meta.dir, '../../../../../contracts/migrations')

/**
 * The directory is read here and nowhere else. `migrations.ts` imports what `index.json`
 * names, because a glob is a different set in a bundle than in a checkout — but a test runs
 * in a checkout, and reading the directory is the only way to notice a file somebody added
 * and forgot to name.
 */
const filesOnDisk = readdirSync(CONTRACT_DIRECTORY, { recursive: true, encoding: 'utf8' })
  .filter((entry) => entry.endsWith('.sql'))
  /* readdirSync gives platform separators; the contract names paths with forward slashes. */
  .map((entry) => entry.split(sep).join('/'))

describe('contracts/migrations', () => {
  // index.json no longer lists the four files of each migration, because they are always
  // called the same thing — it declares that structure once. This is what makes the claim
  // true: every .sql file on disk is one the structure implies, and nothing it implies is
  // missing. A directory with a fifth file, or with `up.sql` instead of `up.sqlite.sql`,
  // fails here rather than at somebody's first start.
  test('the files on disk are exactly the ones the contract implies', () => {
    expect([...MIGRATION_SQL_FILES].sort()).toEqual(filesOnDisk.sort())
  })

  test('every migration has its own directory, named as it is', () => {
    const directories = new Set(filesOnDisk.map((file) => file.split('/')[0]))
    expect([...directories].sort()).toEqual(MIGRATIONS.map((migration) => migration.name).sort())
  })

  test('is numbered from one, ascending, without gaps', () => {
    expect(MIGRATIONS.map((migration) => migration.version)).toEqual(
      MIGRATIONS.map((_migration, position) => position + 1),
    )
  })

  test('has both dialects for every up step, and neither is empty', () => {
    for (const migration of MIGRATIONS) {
      expect(migration.up.postgresql.length).toBeGreaterThan(0)
      expect(migration.up.sqlite.length).toBeGreaterThan(0)
    }
  })

  // Absent is allowed — a migration that drops a column has no inverse — but half of one is
  // not: a down step that exists for PostgreSQL and not for SQLite is a file somebody forgot.
  test('a down step is written for both dialects or for neither', () => {
    for (const migration of MIGRATIONS) {
      if (migration.down === undefined) {
        continue
      }
      expect(migration.down.postgresql.length).toBeGreaterThan(0)
      expect(migration.down.sqlite.length).toBeGreaterThan(0)
    }
  })

  // The import map in migrations.ts is a second list beside index.json, and lists drift.
  // This is the test that turns that drift into a named failure instead of a missing table.
  test('a file named by the contract but not imported fails by name', () => {
    for (const file of MIGRATION_SQL_FILES) {
      expect(filesOnDisk).toContain(file)
    }
  })
})

describe('splitStatements', () => {
  test('splits on the semicolons that end statements', () => {
    expect(splitStatements('CREATE TABLE a (id int);\nCREATE INDEX i ON a (id);\n')).toEqual([
      'CREATE TABLE a (id int)',
      'CREATE INDEX i ON a (id)',
    ])
  })

  test('keeps a statement that spans lines together', () => {
    expect(splitStatements('CREATE TABLE a (\n  id int,\n  b text\n);\n')).toEqual([
      'CREATE TABLE a (\n  id int,\n  b text\n)',
    ])
  })

  test('drops the comment lines that lead up to a statement', () => {
    expect(splitStatements('-- why this exists\nCREATE TABLE a (id int);\n')).toEqual([
      'CREATE TABLE a (id int)',
    ])
  })

  test('drops a trailing comment with no statement after it', () => {
    expect(splitStatements('CREATE TABLE a (id int);\n-- nothing follows\n')).toEqual([
      'CREATE TABLE a (id int)',
    ])
  })

  // The two cases a split(';') gets wrong, and it gets them wrong silently — by handing the
  // driver two halves of one statement.
  test('a semicolon inside a string does not end a statement', () => {
    expect(splitStatements("CREATE TABLE a (b text DEFAULT 'x;y');\n")).toEqual([
      "CREATE TABLE a (b text DEFAULT 'x;y')",
    ])
  })

  test('an escaped quote does not end the string', () => {
    expect(splitStatements("CREATE TABLE a (b text DEFAULT 'it''s; fine');\n")).toEqual([
      "CREATE TABLE a (b text DEFAULT 'it''s; fine')",
    ])
  })

  test('a semicolon inside a dollar-quoted body does not end a statement', () => {
    const sql = 'CREATE FUNCTION f() RETURNS int AS $$ BEGIN RETURN 1; END $$ LANGUAGE plpgsql;\n'
    expect(splitStatements(sql)).toEqual([
      'CREATE FUNCTION f() RETURNS int AS $$ BEGIN RETURN 1; END $$ LANGUAGE plpgsql',
    ])
  })

  test('a tagged dollar body works the same way', () => {
    expect(splitStatements('SELECT $body$ a; b $body$;\n')).toEqual(['SELECT $body$ a; b $body$'])
  })

  test('a lone dollar is not a quote', () => {
    expect(splitStatements('SELECT 1 $ 2;\n')).toEqual(['SELECT 1 $ 2'])
  })

  test('an empty file yields nothing to run', () => {
    expect(splitStatements('\n\n-- only a comment\n')).toEqual([])
  })
})

describe('findSchemaDivergence', () => {
  const step = (version: number, name: string) => ({
    version,
    name,
    up: { postgresql: [], sqlite: [] },
    down: undefined,
  })
  const build = [step(1, '0001_a'), step(2, '0002_b')]

  test('an empty database is not a divergence, it is work to do', () => {
    expect(findSchemaDivergence([], build)).toBeUndefined()
  })

  test('a database part way through is not one either', () => {
    expect(findSchemaDivergence([{ version: 1, name: '0001_a' }], build)).toBeUndefined()
  })

  test('a database that is up to date is not one', () => {
    expect(
      findSchemaDivergence(
        [
          { version: 1, name: '0001_a' },
          { version: 2, name: '0002_b' },
        ],
        build,
      ),
    ).toBeUndefined()
  })

  test('a migration this build does not have names itself and the last shared one', () => {
    const divergence = findSchemaDivergence(
      [
        { version: 1, name: '0001_a' },
        { version: 2, name: '0002_b' },
        { version: 3, name: '0003_c' },
      ],
      build,
    )

    expect(divergence?.applied).toEqual({ version: 3, name: '0003_c' })
    expect(divergence?.expected).toBeUndefined()
    expect(divergence?.agreedUpTo).toEqual({ version: 2, name: '0002_b' })
  })

  // Two branches that both wrote a migration 2. Same version, different history — and a
  // highest-version check would call this database up to date and start serving.
  test('a different name at the same version is a divergence', () => {
    const divergence = findSchemaDivergence(
      [
        { version: 1, name: '0001_a' },
        { version: 2, name: '0002_other' },
      ],
      build,
    )

    expect(divergence?.applied).toEqual({ version: 2, name: '0002_other' })
    expect(divergence?.expected).toEqual({ version: 2, name: '0002_b' })
    expect(divergence?.agreedUpTo).toEqual({ version: 1, name: '0001_a' })
  })

  test('disagreeing from the very first leaves nothing to go back to but empty', () => {
    const divergence = findSchemaDivergence([{ version: 1, name: '0001_other' }], build)

    expect(divergence?.agreedUpTo).toBeUndefined()
    expect(schemaDivergenceMessage(divergence!)).toContain('migrate down to an empty database')
  })
})

describe('schemaDivergenceMessage', () => {
  test('names the migration to go back past, and where its down step lives', () => {
    const message = schemaDivergenceMessage({
      applied: { version: 3, name: '0003_c' },
      expected: undefined,
      agreedUpTo: { version: 2, name: '0002_b' },
    })

    expect(message).toContain('this database has migration 3 "0003_c"')
    expect(message).toContain('which this build does not know')
    expect(message).toContain('Check out the branch the database was built with')
    expect(message).toContain('migrate down to 2 "0002_b"')
  })

  test('says which name each side has when they differ at one version', () => {
    const message = schemaDivergenceMessage({
      applied: { version: 2, name: '0002_other' },
      expected: { version: 2, name: '0002_b' },
      agreedUpTo: { version: 1, name: '0001_a' },
    })

    expect(message).toContain(
      'migration 2 is "0002_other" in this database and "0002_b" in this build',
    )
  })
})
