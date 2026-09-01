import {
  derivedToken,
  type JwtVector,
  loadJwtVectors,
  writeVectorFile,
} from '../packages/contract-tests/src/index.ts'

/**
 * Rewrites the derived fields of `contracts/test-vectors/`, which today is one: the `token` of
 * every jwt vector.
 *
 *     bun run regen:vectors
 *
 * A token is base64url(header).base64url(payload).base64url(HMAC-SHA256), so editing a payload by
 * hand means recomputing an HMAC by hand. This does that and nothing else: no vector is added,
 * removed or reordered, and every field but `token` is left exactly as it was found.
 *
 * Neither suite trusts it. `packages/contract-tests/src/jwt.test.ts` recomputes the same tokens
 * and fails on a disagreement, so a file edited without running this is caught rather than
 * believed — which is also what makes it safe for this to be a convenience rather than a step
 * anyone has to remember.
 *
 * It lives here and not in the package because it prints: `noConsole` is off for `scripts/**`
 * and on everywhere else, and a CLI that cannot say what it did is worse than one that lives a
 * directory away from what it edits. A subject that grows a derived field adds a function here
 * beside `regenerateJwt()`. The path is relative rather than through `@gradido/contract-tests`
 * because the repository root is not itself a workspace member.
 */

function regenerateJwt(): void {
  const { secret, vectors, raw } = loadJwtVectors()
  const rawVectors = (raw as { vectors: JwtVector[] }).vectors

  let rewritten = 0
  for (const [index, vector] of vectors.entries()) {
    const derived = derivedToken(vector, secret)
    if (derived === null || derived === vector.token) {
      continue
    }
    // Written into the parsed file rather than into a new object, so member order — which is the
    // file's readability — survives the round trip.
    const target = rawVectors[index]
    if (target === undefined) {
      throw new Error(`vector ${vector.id} vanished between parse and write`)
    }
    target.token = derived
    rewritten += 1
  }

  if (rewritten === 0) {
    console.log(`jwt.json: ${vectors.length} vectors, every token already the derived one`)
    return
  }
  writeVectorFile('jwt', raw)
  console.log(`jwt.json: rewrote ${rewritten} of ${vectors.length} tokens`)
}

regenerateJwt()
