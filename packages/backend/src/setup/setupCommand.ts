import {
  CommunityRepository,
  createHomeCommunity,
  type DatabaseContext,
} from '@gradido/backend-core'
import type { HomeCommunitySetup } from '@gradido/shared/schemas'

/**
 * The last step of `setup`: write the community row, unless it is already there.
 *
 * Everything before it — the questions, and the `.env` they were written to — has happened
 * by the time this is reached, because the answers decide *which* database this opens. See
 * `main.ts`, `runSetup`, for the order and `askForSetup.ts` for the conversation.
 *
 * `setup` is a command rather than something a serving start does because it asks questions.
 * A process that both answers requests and reads an answer off a terminal is two things at
 * once: it cannot be started unattended, its log and its prompts share a terminal, and under
 * `docker compose up` rather than `run` the prompts go where nobody is looking. Splitting it
 * out is what postgres does with `initdb`, vault with `operator init` and django with
 * `createsuperuser`.
 *
 * **It is safe to run twice.** A database that already has a community says so and stops
 * with 0 — the `.env` written a moment ago is the point of the second run, and an operator
 * changing a relay should not have to drop a row to do it.
 */
export async function setupCommand(
  context: DatabaseContext,
  community: HomeCommunitySetup,
): Promise<void> {
  const existing = await new CommunityRepository(context.db).findHomeCommunity()
  if (existing !== undefined) {
    /* Not a log line: it reports what this invocation found, not something that happened to
       the instance, and the contracted stream has nothing to say about a command that wrote
       no row. The community that is already there was logged the day it was written. */
    process.stderr.write(`this instance is already set up as "${existing.name}"\n`)
    return
  }

  await createHomeCommunity(context, community)
}
