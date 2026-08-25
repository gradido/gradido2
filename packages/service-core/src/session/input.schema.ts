import * as v from 'valibot'

/**
 * Where what someone else wrote becomes something the session store may act on.
 *
 * Everything the store is asked with arrives as JSON from outside: the claims of a token
 * whose signature nobody has checked yet, and the token itself. The rules that turn it into
 * a {@link SessionClaims} live here rather than inside the store, so that *a claim that is
 * absent is not a claim that passed* is one declared schema instead of a line in a method
 * that somebody may one day tidy away.
 *
 * The store's own limits are here for the same reason from the other side: they are what a
 * caller hands in, and a store built with a ceiling of zero would answer every request with
 * a miss and never say why.
 *
 * **Read `Architecture.md`, *Session cache*, before changing any of it.** What each rule is
 * load-bearing for is written there and, in shorter form, beside the rule itself.
 */

/**
 * `users.gradido_id`, the stable public identifier — never the numeric id.
 *
 * The format is checked at both ends, and the second one is the point: a session created
 * under something that is not a uuid could never be found again, because the claim naming
 * it would not get past {@link sessionClaimsSchema}. It would fail silently, as one wasted
 * verification per request.
 */
export const sessionUserUuidSchema = v.pipe(
  v.string('user_uuid must be a string'),
  v.uuid('user_uuid must be a uuid'),
)

/**
 * What a JWT says about its session, spelled as it is spelled in the token.
 *
 * `session_created_at` is unix milliseconds, unlike `iat` and `exp`, which are seconds
 * because RFC 7519 says so. Claims the schema does not name — `iss`, `aud`, `iat`, `exp` —
 * pass through untouched; they belong to the token and are checked where the signature is.
 *
 * **Every rule here is load-bearing, because on this path nothing has been verified yet.**
 * A missing `slot` must not arrive as 0, which is a valid slot and usually holds someone;
 * `v.number()` and `v.integer()` are what turn absent, null, `"3"` and 3.5 into a miss
 * instead. `Architecture.md`, *Safety net*: a claim that is absent is not a claim that
 * passed. Both implementations have to reject the same payloads, which is a contract vector
 * waiting to be written.
 */
export const sessionClaimsSchema = v.pipe(
  v.object({
    slot: v.pipe(
      v.number('slot must be a number'),
      v.integer('slot must be a whole number'),
      v.minValue(0, 'slot cannot be negative'),
    ),
    user_uuid: sessionUserUuidSchema,
    session_created_at: v.pipe(
      v.number('session_created_at must be a number'),
      v.integer('session_created_at must be unix milliseconds'),
      v.minValue(0, 'session_created_at cannot be negative'),
    ),
  }),
  v.transform((claims) => ({
    slot: claims.slot,
    userUuid: claims.user_uuid,
    sessionCreatedAt: claims.session_created_at,
  })),
)

/** What a token carries, before anyone has looked at it: the claims as they are spelled there. */
export type SessionClaimsInput = v.InferInput<typeof sessionClaimsSchema>

/**
 * The identity of one session, and a type that says it has been through the schema.
 *
 * A value of this type cannot be a payload someone handed in: the schema renames all three
 * claims on the way through, so only its output has these names. Anything typed with it has
 * therefore been parsed, and **nothing that reads it may check it again** — the rules are in
 * the schema, and a second copy of them in a function drifts from the first.
 *
 * These are session claims, not token claims. A token is re-minted once it is older than
 * the re-issue interval and the new one copies all three unchanged, so a login moves while
 * a session does not: `session_created_at` keeps counting towards the hard timeout through
 * any number of refreshes. That is what makes the timeout a backstop against
 * cache-invalidation bugs rather than a login timeout — a token that could stamp itself
 * afresh would keep a stale working set alive forever.
 */
export type SessionClaims = v.InferOutput<typeof sessionClaimsSchema>

/**
 * Turns the payload of a token nobody has verified into claims, or into nothing.
 *
 * This is the one place where a parsed JWT body becomes a {@link SessionClaims}, and it is
 * the reason the store's methods can take a typed argument at all. A payload that does not
 * fit is not an error: it is a token this process cannot answer from memory, which is a
 * miss like any other, and the caller verifies a signature instead.
 *
 * Measured against what it stands in for, on a Ryzen 7 5700G under bun 1.3.14: this parse
 * is 281 ns, the HMAC it lets the hit path skip is 852 ns, and the base64 decode and
 * `JSON.parse` that produce its input are 1051 ns and are paid by both paths.
 */
export function readSessionClaims(payload: unknown): SessionClaims | undefined {
  const parsed = v.safeParse(sessionClaimsSchema, payload)
  return parsed.success ? parsed.output : undefined
}

/** Spells claims back out for a token — the other half of {@link readSessionClaims}. */
export function writeSessionClaims(claims: SessionClaims): SessionClaimsInput {
  return {
    slot: claims.slot,
    user_uuid: claims.userUuid,
    session_created_at: claims.sessionCreatedAt,
  }
}

/**
 * What the store refuses to be built with. The limits only — the logger and the clock are
 * objects, not parameters a schema has anything to say about.
 *
 * `maxSessions` is a ceiling, **not a sizing decision**. How many sessions live at once is
 * the number created within one `hardTimeoutMs`, which depends on the community and the hour,
 * and the store finds it by itself: it appends a slot when it needs one and reuses the slots
 * of sessions that have ended. Nothing is retired early below this line. What the number is
 * for is a load nobody planned for ending the process instead of the request — and what it
 * should be is still open, because it is a memory question: `Architecture.md`, *Session
 * cache*, has the experiment that answers it.
 *
 * `tokenReissueAfterMs` lives here rather than at the caller because it is what bounds the
 * token set: one token per interval for as long as a session lives. Deciding it from the
 * token's own `iat` instead would hand that bound to whoever writes the token.
 */
export const sessionStoreLimitsSchema = v.object({
  maxSessions: v.pipe(
    v.number('maxSessions must be a number'),
    v.integer('maxSessions must be a whole number of sessions'),
    v.minValue(1, 'maxSessions must leave room for at least one session'),
  ),
  hardTimeoutMs: v.pipe(
    v.number('hardTimeoutMs must be a number'),
    v.integer('hardTimeoutMs must be whole milliseconds'),
    v.minValue(1, 'hardTimeoutMs must be positive'),
  ),
  tokenReissueAfterMs: v.pipe(
    v.number('tokenReissueAfterMs must be a number'),
    v.integer('tokenReissueAfterMs must be whole milliseconds'),
    v.minValue(1, 'tokenReissueAfterMs must be positive, or it would not bound the token set'),
  ),
})

/** What a caller hands the store: the limits as written down, before they are checked. */
export type SessionStoreLimitsInput = v.InferInput<typeof sessionStoreLimitsSchema>

/** The limits the store runs on, which is to say the ones that came back from `v.parse`. */
export type SessionStoreLimits = v.InferOutput<typeof sessionStoreLimitsSchema>

/**
 * A token, as the client presented it or as `mint` returned it.
 *
 * The only rule is that there is one: a session whose token is empty is a session nothing
 * can ever reach again, and it would fail silently, one wasted verification per request.
 */
export const sessionTokenSchema = v.pipe(
  v.string('a session token must be a string'),
  v.nonEmpty('a session cannot be reached without a token'),
)
