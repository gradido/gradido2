import { type DatabaseConnection, MIGRATIONS, migrateDown } from '@gradido/backend-core'
import type { Logger } from '@gradido/service-core'
import { CONFIG } from '../config'

/** What `DB_MIGRATE_DOWN` is set to when the step to undo is the first one. */
export const EMPTY_DATABASE = '0'

/**
 * `migrate-down` — takes the database down by one migration.
 *
 * **In development it runs. On a release it runs only when `DB_MIGRATE_DOWN` names the
 * migration one lower** — the version the database is to end at, not the one being undone.
 * `0` means an empty database, which is what "one lower" is when the first migration is the
 * one going away.
 *
 * A name and not a yes: it matches only when the target really is one step below where the
 * database is now, so a value left behind in an env file cannot take the next step too — the
 * moment it has been reached it stops meaning "one lower". And it says which state was meant,
 * which for a one-step operation is the whole confirmation.
 *
 * It is a command rather than something a normal start does, and the reason is not a rule
 * about servers: a serving process migrates *up* to the version its code needs, so going down
 * to N-1 and then serving a build that requires N are contradictory in one process — it would
 * undo the step and immediately re-apply it. Going down means the next thing started is a
 * different build.
 */
export async function migrateDownCommand(
  connection: DatabaseConnection,
  logger: Logger,
): Promise<void> {
  const release = CONFIG.NODE_ENV === 'production'
  if (!release) {
    /* Said before it happens, so a developer meets the variable long before the day they
       need it on a release. Not a contracted event: it reports how this run was invoked,
       nothing about the database. */
    process.stderr.write(
      'migrating down without confirmation, which only development allows; a release needs DB_MIGRATE_DOWN set to the migration one lower\n',
    )
  }
  if (release && CONFIG.DB_MIGRATE_DOWN === '') {
    throw new Error(
      `refusing to migrate down on a release without confirmation. Set DB_MIGRATE_DOWN to the migration the database should end at — one lower than where it is, or ${EMPTY_DATABASE} for an empty database. This destroys whatever the migration being undone held; read its down file in contracts/migrations first.`,
    )
  }

  await migrateDown(connection, logger, {
    /* Handed in rather than compared afterwards: which migration is at the head is a
       property of the database, so only migrateDown can hold the confirmation against it
       before doing anything. A comparison after the fact reports what already happened. */
    target: release ? CONFIG.DB_MIGRATE_DOWN : undefined,
  })
}

/** The migration one below the last this build carries — what a release would confirm with. */
export function suggestedTarget(): string {
  return MIGRATIONS.at(-2)?.name ?? EMPTY_DATABASE
}
