import { describe, expect, test } from 'bun:test'
import { newEmailVerificationCode } from './verificationCode.logic'

/** contracts/db/user_contacts.json — email_verification_code, range [1, 2^53-1]. */
const MAX_CODE = 9007199254740991n

describe('newEmailVerificationCode', () => {
  test('stays inside what both databases can hold, over many draws', () => {
    for (let index = 0; index < 20000; index++) {
      const code = newEmailVerificationCode()
      expect(code).toBeGreaterThan(0n)
      expect(code).toBeLessThanOrEqual(MAX_CODE)
    }
  })

  // A code that came back from SQLite rounded is a link that does not work, with nothing
  // failing anywhere. The bound is what prevents it, so this is the test for the bound.
  test('survives the trip through a double unchanged', () => {
    for (let index = 0; index < 20000; index++) {
      const code = newEmailVerificationCode()
      expect(BigInt(Number(code))).toBe(code)
    }
  })

  test('draws different codes', () => {
    const codes = new Set<bigint>()
    for (let index = 0; index < 1000; index++) {
      codes.add(newEmailVerificationCode())
    }
    expect(codes.size).toBe(1000)
  })

  test('uses the whole width rather than a corner of it', () => {
    let seen = 0n
    for (let index = 0; index < 1000; index++) {
      seen |= newEmailVerificationCode()
    }
    // Every one of the 53 bits turned up at least once in a thousand draws.
    expect(seen).toBe(MAX_CODE)
  })
})
