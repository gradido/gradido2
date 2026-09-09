import {
  CommunityRepository,
  createHomeCommunity,
  type DatabaseContext,
} from '@gradido/backend-core'
import { askForHomeCommunity, canAskForHomeCommunity } from './askForHomeCommunity'
import { SetupError } from './requireHomeCommunity'

/**
 * `setup` — say who this community is, once, and stop.
 *
 * This is the conversation that used to happen inside a serving start. It is a command for the
 * same kind of reason `migrate-down` is one, though not the same reason: a server's job is to
 * answer requests, and a process that is also reading an answer from a terminal is two things at
 * once. It cannot be started by an orchestrator without somebody watching it, its log and its
 * questions share a terminal, and the moment it is run under `docker compose up` rather than
 * `run` the prompt goes somewhere nobody is looking. Splitting it out is what postgres does with
 * `initdb`, vault with `operator init` and django with `createsuperuser`.
 *
 * **It is safe to run twice.** A database that already has a community says so and stops with 0 —
 * setting up an instance that is set up is not a failure, it is a no-op, and an operator running
 * this from a script should not have to check first.
 *
 * The migrations run first, because the row cannot be written into a schema that is not there
 * yet. That is the same thing a serving start does and it is idempotent, so nothing is decided
 * here that `serve` would have decided differently.
 */
export async function setupCommand(context: DatabaseContext): Promise<void> {
  const existing = await new CommunityRepository(context.db).findHomeCommunity()
  if (existing !== undefined) {
    /* Not a log line: it reports what this invocation found, not something that happened to
       the instance, and the contracted stream has nothing to say about a command that did
       nothing. The community that is already there was logged the day it was written. */
    process.stderr.write(`this instance is already set up as "${existing.name}"\n`)
    return
  }

  if (!canAskForHomeCommunity()) {
    throw new SetupError(
      'no-terminal',
      [
        'setup needs a terminal to ask on, and there is none.',
        'Run it with one attached —',
        'under docker compose that is: docker compose run --rm backend setup',
      ].join(' '),
    )
  }

  /* Asks whatever is buffered to go out before the questions start — the migration lines
     usually are. On a terminal the log writes synchronously anyway (see logging/logger.ts,
     WATCHED), so this is the belt to that pair of braces rather than the only thing holding
     them apart. */
  context.logger.flush()
  await createHomeCommunity(context, await askForHomeCommunity())
}
