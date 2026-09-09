import {
  CommunityRepository,
  type DatabaseContext,
  type HomeCommunity,
} from '@gradido/backend-core'

/**
 * The instance cannot be set up, which is a different failure from the database being
 * unreachable and is logged as one — `startup.setup.failed`, not `startup.database.failed`.
 * The database answered fine; there is simply a step nobody has taken.
 */
export class SetupError extends Error {
  public constructor(
    /** Closed vocabulary, see contracts/logging.json: `not-set-up`, `no-terminal`. */
    public readonly reason: string,
    message: string,
  ) {
    super(message)
    this.name = 'SetupError'
  }
}

/**
 * The community this instance is, read from the database.
 *
 * There is no second case here and that is the point: **a serving start never asks.** An empty
 * database ends the start with a line naming the `setup` command, and somebody runs that
 * command with a terminal attached.
 *
 * It used to ask on the spot, and moving it out is worth the extra step for two reasons. A
 * process that prompts is a process that can hang waiting for an answer nobody is there to
 * give — the TTY check caught the plain cases, but not a terminal that goes away mid-answer, and
 * `docker compose up` attaches one. And a server that writes its log to the same terminal it is
 * reading an answer from has to keep the two apart forever after; the standard answer, which
 * postgres, vault, django and gitea all give in one form or another, is that the serving process
 * simply is not the one holding the conversation. See `setupCommand.ts`.
 *
 * There is no third case either, because `users.community_id` is NOT NULL: without this row
 * nothing can register, so serving without it would only mean failing later and less clearly.
 */
export async function requireHomeCommunity(context: DatabaseContext): Promise<HomeCommunity> {
  const existing = await new CommunityRepository(context.db).findHomeCommunity()
  if (existing !== undefined) {
    return existing
  }

  throw new SetupError(
    'not-set-up',
    [
      'this database has no community yet.',
      'Run the setup command once, with a terminal attached, to say who this community is —',
      'under docker compose that is: docker compose run --rm backend setup',
    ].join(' '),
  )
}
