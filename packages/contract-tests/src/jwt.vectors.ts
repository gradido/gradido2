import { createHmac } from 'node:crypto'
import * as v from 'valibot'
import { contractValueSchema, loadVectors } from './vectors.ts'

/**
 * The shape of `contracts/test-vectors/jwt.json`, and the one derivation in it.
 *
 * Shared by the runner and by the regenerator, so that the rule a token is built under is
 * written once. The C runner reads the same file and derives nothing — it takes `token` as the
 * bytes that arrive over the wire, which is all a verifier ever gets.
 */

/** What the TypeScript path produces where it cannot meet `expect`. See the file's own notes. */
const divergenceSchema = v.object({
  accepted: v.boolean(),
  claim: v.nullable(v.string()),
  why: v.pipe(v.string(), v.nonEmpty('a divergence without a reason is an excuse')),
})

export const jwtVectorSchema = v.object({
  id: v.string(),
  why: v.pipe(v.string(), v.nonEmpty('every vector says what it is load bearing for')),
  claimName: v.string(),
  issuer: v.nullable(v.string()),
  audience: v.nullable(v.string()),
  /** unix seconds, as a decimal string — `contracts/AGENTS.md`, *Numbers are decimal strings*. */
  now: v.pipe(v.string(), v.regex(/^-?\d+$/, 'now is unix seconds as a decimal string')),
  /** Exact bytes, not an object: a vector has to be able to spell one that is not valid JSON. */
  header: v.nullable(v.string()),
  payload: v.nullable(v.string()),
  tamperSignature: v.boolean(),
  token: v.string(),
  expect: v.object({ accepted: v.boolean(), claim: v.nullable(v.string()) }),
  c: v.object({ result: v.pipe(v.string(), v.startsWith('SC_JWT_')) }),
  typescript: v.nullable(divergenceSchema),
})

export type JwtVector = v.InferOutput<typeof jwtVectorSchema>

const base64url = (bytes: Uint8Array | Buffer): string => Buffer.from(bytes).toString('base64url')

/**
 * The token a vector's `header`, `payload` and `tamperSignature` describe.
 *
 * A vector whose header and payload are null carries a token that is not of that shape at all —
 * one that is malformed on purpose — and nothing is derived for it.
 *
 * @returns the token, or null where the vector derives none
 */
export function derivedToken(vector: JwtVector, secret: Uint8Array): string | null {
  if (vector.header === null || vector.payload === null) {
    return null
  }

  const signedInput = `${base64url(Buffer.from(vector.header))}.${base64url(Buffer.from(vector.payload))}`
  const mac = createHmac('sha256', secret).update(signedInput).digest()
  const signature = base64url(mac)
  if (!vector.tamperSignature) {
    return `${signedInput}.${signature}`
  }

  // One flipped character, which is the smallest thing that can be wrong with a signature and
  // therefore the one worth pinning. Deterministic, so the file does not change between runs.
  const last = signature.slice(-1)
  return `${signedInput}.${signature.slice(0, -1)}${last === 'A' ? 'B' : 'A'}`
}

export interface JwtVectorFile {
  readonly secret: Uint8Array
  readonly claimMaxBytes: number
  readonly vectors: JwtVector[]
  /** The parsed file as it stands on disk, for a regenerator that writes it back. */
  readonly raw: Record<string, unknown>
}

/** Reads `jwt.json`, checks its envelope, and hands back what a runner needs from it. */
export function loadJwtVectors(): JwtVectorFile {
  const { file, vectors } = loadVectors('jwt', jwtVectorSchema)
  const secret = v.parse(contractValueSchema, (file as { secret: unknown }).secret)
  const rules = (file as { rules: Record<string, unknown> }).rules
  const claimMaxBytes = v.parse(contractValueSchema, rules.claimMaxBytes)

  return {
    secret: new TextEncoder().encode(secret.value),
    claimMaxBytes: Number(claimMaxBytes.value),
    vectors,
    raw: file,
  }
}
