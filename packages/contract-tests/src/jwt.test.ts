import { describe, expect, test } from 'bun:test'
import { verifyHs256 } from './jwt.reference.ts'
import { derivedToken, loadJwtVectors } from './jwt.vectors.ts'

/**
 * `contracts/test-vectors/jwt.json`, run against the TypeScript path.
 *
 * The other half of this suite is `fast-servers/tests/contract/test_jwt_contract.cpp`, which
 * runs the same file against `jwt.c`. Neither half is the authority: the file is, and each
 * implementation is measured against it rather than against the other. That is what makes a
 * disagreement show up as one named failure instead of as two suites that are both green.
 *
 * Three things this suite does that a plain "for each vector, verify" would not:
 *
 * - **It rebuilds every token from `header` and `payload`.** The token in the file is derived,
 *   and a payload edited without regenerating it would otherwise go on testing the old bytes.
 * - **It asserts the declared divergences.** A vector's `typescript` block says where this path
 *   cannot meet the contract; the assertion is that it really cannot. One that gets fixed
 *   upstream fails here until the block is deleted, so the list of known gaps cannot rot.
 * - **It counts.** Every vector runs, and the count is checked against what the file declares —
 *   `loadVectors()` does that. A vector nobody runs is a disagreement nobody reports.
 */

const { secret, claimMaxBytes, vectors } = loadJwtVectors()

describe('contract vectors: jwt', () => {
  test('the file carries vectors and every one of them is a case', () => {
    expect(vectors.length).toBeGreaterThan(0)
  })

  describe('the tokens are the ones the header and payload describe', () => {
    for (const vector of vectors) {
      const derived = derivedToken(vector, secret)
      test.if(derived !== null)(vector.id, () => {
        expect(vector.token).toBe(derived as string)
      })
    }
  })

  describe('verification', () => {
    for (const vector of vectors) {
      test(vector.id, async () => {
        const result = await verifyHs256(
          { secret, issuer: vector.issuer, audience: vector.audience, claimMaxBytes },
          vector.token,
          vector.claimName,
          Number(vector.now),
        )

        // A `typescript` block replaces the contract for this path, and only for it. Its `why`
        // is what a reader of a red suite needs, so it goes into the message.
        const wanted = vector.typescript ?? vector.expect
        const because = vector.typescript
          ? `\n  known divergence: ${vector.typescript.why}`
          : `\n  ${vector.why}`

        expect(
          { accepted: result.accepted, claim: result.claim, id: vector.id },
          `${vector.id} refused as ${result.reason}${because}`,
        ).toEqual({ accepted: wanted.accepted, claim: wanted.claim, id: vector.id })
      })
    }
  })

  describe('the declared divergences are still real', () => {
    const diverging = vectors.filter((vector) => vector.typescript !== null)

    test('there are as few of them as the file says', () => {
      // Not a bound anyone should raise casually: every entry here is a token the two
      // implementations answer differently, and the list is meant to shrink.
      expect(diverging.map((vector) => vector.id)).toEqual([
        'jwt.verify.refuses-a-list-valued-audience',
      ])
    })

    for (const vector of diverging) {
      test(`${vector.id} still cannot meet the contract`, async () => {
        const result = await verifyHs256(
          { secret, issuer: vector.issuer, audience: vector.audience, claimMaxBytes },
          vector.token,
          vector.claimName,
          Number(vector.now),
        )
        // If this fails, the gap closed: delete the `typescript` block from the vector and let
        // both implementations be held to `expect` again.
        expect(
          result.accepted,
          `${vector.id} now agrees with the contract — delete its typescript block`,
        ).not.toBe(vector.expect.accepted)
      })
    }
  })
})
