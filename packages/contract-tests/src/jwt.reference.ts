import { errors, jwtVerify } from 'jose'

/**
 * The TypeScript half of the JWT contract: jose, plus the rules jose does not have an opinion
 * about.
 *
 * This is the reference implementation of what `fast-servers/service-core/src/jwt.c` does, and
 * it is deliberately a *whole* one rather than a call to `jwtVerify()` in the test. jose
 * verifies a token; it does not know that this project requires `exp`, or that it wants one
 * particular claim copied out, or how long that claim may be. Those rules are the contract, and
 * a rule that lives in a test rather than in code is a rule the real backend will not have.
 *
 * When `packages/service-core` grows a session verifier, it replaces this file and the runner
 * imports that instead. Everything here is written the way that verifier would be.
 *
 * The two things to know about the shape below:
 *
 * - **`requiredClaims: ['exp']` is not optional.** jose accepts a token with no expiry at all,
 *   which for this project would be a session that never ends —
 *   `../../../fast-servers/Architecture.md`, *Safety net*. The contract vector
 *   `jwt.verify.refuses-a-token-with-no-exp` is what stops that line from being deleted.
 * - **A refusal carries a reason and the reason is not contracted.** Every one of them is one
 *   401 with one body, so a caller cannot tell them apart; the two implementations check in a
 *   different order and would report different ones for the same token. It is here so a log
 *   line and a test failure can say something, and nothing asserts it across the two.
 */

/** What a refusal is called. The spelling is C's `sc_jwt_result`, minus the `SC_JWT_` prefix. */
export type JwtReason =
  | 'OK'
  | 'MALFORMED'
  | 'BAD_ALGORITHM'
  | 'BAD_SIGNATURE'
  | 'EXPIRED'
  | 'BAD_ISSUER'
  | 'BAD_AUDIENCE'
  | 'MISSING_CLAIM'

export interface JwtVerifyConfig {
  readonly secret: Uint8Array
  /** Optional, and null skips the check rather than accepting an absent claim. */
  readonly issuer: string | null
  readonly audience: string | null
  /** Longest claim value accepted, in bytes. `rules.claimMaxBytes` of the vector file. */
  readonly claimMaxBytes: number
}

export interface JwtVerifyResult {
  readonly accepted: boolean
  /** The claim value on acceptance, null on every refusal. */
  readonly claim: string | null
  /** Diagnostic. Not contracted across the two implementations — see the note above. */
  readonly reason: JwtReason
}

function refused(reason: JwtReason): JwtVerifyResult {
  return { accepted: false, claim: null, reason }
}

/** Which refusal jose's error is, as far as it can be told from outside. */
function reasonOf(error: unknown): JwtReason {
  if (error instanceof errors.JWTExpired) {
    return 'EXPIRED'
  }
  if (error instanceof errors.JOSEAlgNotAllowed) {
    return 'BAD_ALGORITHM'
  }
  if (error instanceof errors.JWSSignatureVerificationFailed) {
    return 'BAD_SIGNATURE'
  }
  if (error instanceof errors.JWTClaimValidationFailed) {
    if (error.claim === 'iss') {
      return 'BAD_ISSUER'
    }
    if (error.claim === 'aud') {
      return 'BAD_AUDIENCE'
    }
    return 'MISSING_CLAIM'
  }
  // JWSInvalid covers a token that is not three segments and a header that is not a JOSE one;
  // JWTInvalid covers a payload that is not a claims set. Both are "these bytes are not a token
  // this verifier can read", which is what MALFORMED says.
  if (error instanceof errors.JWSInvalid || error instanceof errors.JWTInvalid) {
    return 'MALFORMED'
  }
  throw error
}

/**
 * Verifies the signature, the expiry, the issuer and the audience, then copies out @p claimName.
 *
 * @param now unix seconds, passed in rather than read off a clock so a vector can pin a boundary
 */
export async function verifyHs256(
  config: JwtVerifyConfig,
  token: string,
  claimName: string,
  now: number,
): Promise<JwtVerifyResult> {
  let payload: Record<string, unknown>
  try {
    const verified = await jwtVerify(token, config.secret, {
      algorithms: ['HS256'],
      // Absent, null and a string all have to be a refusal rather than a token that never
      // expires. jose asks for the claim to be there; its own type check does the rest.
      requiredClaims: ['exp'],
      ...(config.issuer === null ? {} : { issuer: config.issuer }),
      ...(config.audience === null ? {} : { audience: config.audience }),
      currentDate: new Date(now * 1000),
      // No leeway. The C path compares `exp <= now` with nothing added to either side, and a
      // tolerance here would move the boundary the two boundary vectors pin.
      clockTolerance: 0,
    })
    payload = verified.payload as Record<string, unknown>
  } catch (error) {
    return refused(reasonOf(error))
  }

  // What jose has no notion of: the one claim this project reads out of a session token. A claim
  // that is absent is not a claim that passed, and the C default for a missing string is an
  // empty one, which is a value something downstream would act on.
  const value = payload[claimName]
  if (typeof value !== 'string' || value.length === 0) {
    return refused('MISSING_CLAIM')
  }
  // Bytes and not characters: the fast path copies into a fixed buffer and measures what it
  // copies. A claim of astral characters is longer than its length says.
  if (new TextEncoder().encode(value).length > config.claimMaxBytes) {
    return refused('MISSING_CLAIM')
  }

  return { accepted: true, claim: value, reason: 'OK' }
}
