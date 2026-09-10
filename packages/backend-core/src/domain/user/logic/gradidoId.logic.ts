/**
 * A value for `users.gradido_id`.
 *
 * **Unique per community, not globally** — `contracts/db/users.json`, `uuid_key`. This draws
 * and hands over; it does not ask whether the value is free. `users_uuid_key` answers that
 * question at the moment of the write, which is the only moment the answer is still true, and
 * a lookup before it would be a round trip spent on an answer that can go stale in between.
 *
 * So a collision is not prevented here, it is *survived*: `registerAccount` catches the unique
 * violation, says so in the log and draws again. At 122 random bits from the system CSPRNG that
 * path is not one anybody will see — which is exactly why it must be written down rather than
 * assumed away, because nothing that never runs is ever noticed to be wrong.
 *
 * The asymmetry with `alias` is the reason this exists as code rather than as a constraint
 * alone: a *generated* value that collides is drawn again and nobody notices, while a *chosen*
 * alias that collides is a person being told no.
 */

/* string, not the template literal type crypto.randomUUID() infers: what leaves here is a
   column value, and `users.gradido_id` is a uuid to the database, not a shape to TypeScript. */
export function newGradidoId(): string {
  return crypto.randomUUID()
}
