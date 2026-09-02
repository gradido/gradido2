import {
  CommunityRepository,
  createHomeCommunity,
  type DatabaseContext,
  type HomeCommunity,
} from '@gradido/backend-core'
import { askForHomeCommunity, canAskForHomeCommunity } from './askForHomeCommunity'

/**
 * The instance cannot be set up, which is a different failure from the database being
 * unreachable and is logged as one — `startup.setup.failed`, not `startup.database.failed`.
 * The database answered fine; there is simply nobody to ask who this community is.
 */
export class SetupError extends Error {
  public constructor(
    /** Closed vocabulary, see contracts/logging.json: `no-terminal`. */
    public readonly reason: string,
    message: string,
  ) {
    super(message)
    this.name = 'SetupError'
  }
}

/**
 * The community this instance is — read from the database, or set up on the spot.
 *
 * This is the one place the two possible first moments of a Gradido server meet: a database
 * that has been through this before answers immediately, and an empty one turns the start
 * into a short conversation. There is no third case, because `users.community_id` is NOT
 * NULL: without this row nothing can register, so serving without it would only mean failing
 * later and less clearly.
 */
export async function resolveHomeCommunity(context: DatabaseContext): Promise<HomeCommunity> {
  const existing = await new CommunityRepository(context.db).findHomeCommunity()
  if (existing !== undefined) {
    return existing
  }

  if (!canAskForHomeCommunity()) {
    /* No terminal, so nobody to ask. Blocking on a prompt nobody can see would look like a
       hung start; this says what it is and what to do about it. */
    throw new SetupError(
      'no-terminal',
      [
        'this database has no community yet, and there is no terminal to ask on.',
        'Start the backend once with a terminal attached to set it up —',
        'under docker compose that is: docker compose run --rm backend',
      ].join(' '),
    )
  }

  /* Asks whatever is buffered to go out before the questions start. It is not a guarantee:
     the log transport runs on its own thread, so a line can still surface between two
     prompts — the migration lines usually do. Worth doing anyway, and not worth a sleep. */
  context.logger.flush()
  return createHomeCommunity(context, await askForHomeCommunity())
}
