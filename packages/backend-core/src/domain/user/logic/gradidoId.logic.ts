/**
 * A `users.gradido_id` nobody in this community holds yet.
 *
 * **Unique per community, not globally** — `contracts/db/users.json`, `uuid_key`. A v4 uuid
 * from the system CSPRNG makes a collision unlikely, and unlikely is not impossible: the
 * check is what makes the rule true rather than probable, and it costs one indexed lookup on
 * a path that is already writing three rows.
 *
 * The asymmetry with `alias` is the whole reason this exists as code rather than as a
 * constraint alone: a *generated* value that collides is simply drawn again and nobody
 * notices, while a *chosen* alias that collides is a person being told no. Legacy does the
 * same loop, in `newGradidoID`.
 *
 * `exists` is a parameter rather than a repository call, so the ladder can be tested without
 * a database and so this file stays free of persistence.
 */

/**
 * How many draws before giving up.
 *
 * Reaching this is not a collision — at 122 random bits, five in a row is not a number that
 * happens. It is `exists` answering true for reasons of its own, and a loop that would spin
 * forever on it is worse than an error that says so.
 */
const MAX_DRAWS = 5

/* Promise<string>, not the template literal type crypto.randomUUID() infers: what leaves
   here is a column value, and `users.gradido_id` is a uuid to the database, not a shape to
   TypeScript. */
export async function newGradidoId(
  exists: (gradidoId: string) => Promise<boolean>,
): Promise<string> {
  for (let draw = 0; draw < MAX_DRAWS; draw++) {
    const gradidoId = crypto.randomUUID()
    if (!(await exists(gradidoId))) {
      return gradidoId
    }
  }
  throw new Error(`no free gradido_id after ${MAX_DRAWS} draws`)
}
