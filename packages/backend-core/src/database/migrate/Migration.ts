/**
 * One schema step, as this implementation runs it.
 *
 * **The migrations themselves live in `contracts/migrations`, not here.** The fast path in C
 * opens the same database and has to build the same schema; a migration written in
 * TypeScript would be a schema only one implementation knows how to create, and the other
 * would have to transcribe it and stay transcribed. So the SQL is a plain `.sql` file that
 * either language can read, the order is a JSON file either language can parse, and this
 * type is only what the file becomes after `migrations.ts` has loaded it.
 *
 * A migration is never edited once it has run anywhere. It is followed by another one.
 */
export interface Migration {
  /** The number this migration is known by. Ascending, no gaps, never reused. */
  readonly version: number
  /** What goes into `migrations.file_name`, so a row names something findable. */
  readonly name: string
  /** Statements in order. Executed inside one transaction; both databases roll DDL back. */
  readonly up: DialectStatements
  /**
   * How to undo it, when it can be undone.
   *
   * Absent is a real answer, not a missing file: a migration that drops a column has no
   * inverse, because the values are gone. `contracts/migrations/index.json` says which it is
   * and why. Nothing executes these yet — see the open question there.
   */
  readonly down: DialectStatements | undefined
}

/** The same step, spelled for each database. */
export interface DialectStatements {
  readonly postgresql: readonly string[]
  readonly sqlite: readonly string[]
}

/**
 * Splits a `.sql` file into the statements it holds.
 *
 * The rule is in `contracts/migrations/index.json` because both implementations have to
 * split the same way: a semicolon ends a statement when it ends a line, and a semicolon
 * inside a string or a dollar-quoted body does not. Without the second half this would be a
 * `split(';')` that breaks the first time a migration writes `DEFAULT 'a;b'` or a PostgreSQL
 * function body — quietly, by handing the driver two halves of one statement.
 *
 * It is deliberately not a SQL parser. It knows about the two things that can contain a
 * semicolon and nothing else about the language.
 */
export function splitStatements(sql: string): string[] {
  const statements: string[] = []
  let statement = ''
  let index = 0

  while (index < sql.length) {
    const character = sql[index]

    if (character === "'") {
      const end = closingQuote(sql, index)
      statement += sql.slice(index, end)
      index = end
      continue
    }

    if (character === '$') {
      const tag = dollarTag(sql, index)
      if (tag !== undefined) {
        const end = sql.indexOf(tag, index + tag.length)
        /* An unterminated body is a broken migration; take the rest and let the database
           say so, rather than guessing where it was meant to end. */
        const stop = end === -1 ? sql.length : end + tag.length
        statement += sql.slice(index, stop)
        index = stop
        continue
      }
    }

    if (character === '-' && sql.startsWith('--', index)) {
      const end = sql.indexOf('\n', index)
      const stop = end === -1 ? sql.length : end
      statement += sql.slice(index, stop)
      index = stop
      continue
    }

    if (character === ';') {
      statements.push(statement)
      statement = ''
      index += 1
      continue
    }

    statement += character
    index += 1
  }

  statements.push(statement)
  return statements.map(withoutComments).filter((entry) => entry !== '')
}

/** The index just past the closing quote of the string starting at `start`. */
function closingQuote(sql: string, start: number): number {
  let index = start + 1
  while (index < sql.length) {
    if (sql[index] === "'") {
      /* '' inside a string is an escaped quote, not the end of one. */
      if (sql[index + 1] === "'") {
        index += 2
        continue
      }
      return index + 1
    }
    index += 1
  }
  return sql.length
}

/** `$$` or `$name$` at this position, or nothing if the `$` is something else. */
function dollarTag(sql: string, start: number): string | undefined {
  const match = /^\$[A-Za-z_][A-Za-z0-9_]*\$|^\$\$/u.exec(sql.slice(start))
  return match?.[0]
}

/**
 * The statement without the comment lines that led up to it.
 *
 * They are kept in the file because that is where the reasoning belongs, and dropped here
 * because a statement that is nothing but comments is not a statement — the last "statement"
 * of every file is exactly that, everything after the final semicolon.
 */
function withoutComments(statement: string): string {
  return statement
    .split('\n')
    .filter((line) => !line.trimStart().startsWith('--'))
    .join('\n')
    .trim()
}

/** A row of the `migrations` table: what this database says has been applied to it. */
export interface AppliedMigration {
  readonly version: number
  readonly name: string
}

/** Where a database and this build stop telling the same story. */
export interface SchemaDivergence {
  /** The first applied migration this build cannot account for. */
  readonly applied: AppliedMigration
  /** What this build has at that version. Absent when the database is simply ahead. */
  readonly expected: AppliedMigration | undefined
  /** The last version both agree on, or nothing when they disagree from the first. */
  readonly agreedUpTo: AppliedMigration | undefined
}

/**
 * Whether this build may run against a database, given what has been applied to it.
 *
 * The applied migrations must be a **prefix** of the ones this build carries. Fewer is not a
 * problem — that is work to do, and `runMigrations` does it. More, or a different name at the
 * same version, is: it means the database was built by code this one is not, and applying
 * anything on top of it would produce a schema neither branch describes.
 *
 * Legacy refuses in both directions, because it does not migrate on startup; it compares the
 * highest filename and says "the backend requires X but found Y". This keeps the refusal and
 * drops the half that is now ordinary work, and it names the migration where the two part
 * company rather than only the two ends — that is the one a person has to migrate down past.
 */
export function findSchemaDivergence(
  applied: readonly AppliedMigration[],
  migrations: readonly Migration[],
): SchemaDivergence | undefined {
  for (const [position, row] of applied.entries()) {
    const known = migrations[position]
    if (known !== undefined && known.version === row.version && known.name === row.name) {
      continue
    }
    return {
      applied: row,
      expected: known === undefined ? undefined : { version: known.version, name: known.name },
      agreedUpTo: position === 0 ? undefined : applied[position - 1],
    }
  }
  return undefined
}

/**
 * What to tell whoever started this process.
 *
 * The useful sentence is not "versions differ", it is *which* migration to get back past and
 * that the code able to undo it is in the branch that applied it — this build has no down
 * step for a migration it does not have. Down migrations are not part of the contract format
 * yet; see `contracts/migrations/index.json`.
 */
export function schemaDivergenceMessage(divergence: SchemaDivergence): string {
  const { applied, expected, agreedUpTo } = divergence
  const found = `${applied.version} "${applied.name}"`
  const target =
    agreedUpTo === undefined ? 'an empty database' : `${agreedUpTo.version} "${agreedUpTo.name}"`

  const what =
    expected === undefined
      ? `this database has migration ${found}, which this build does not know`
      : `migration ${applied.version} is "${applied.name}" in this database and "${expected.name}" in this build`

  return `${what}. Check out the branch the database was built with and migrate down to ${target}, then start this build again — or run a build that includes ${found}.`
}
