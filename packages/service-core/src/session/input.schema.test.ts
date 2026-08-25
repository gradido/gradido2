import { describe, expect, test } from 'bun:test'
import { readSessionClaims, type SessionClaims, writeSessionClaims } from './input.schema'

/** users.gradido_id is a uuid, so the tests name their users the way the database does. */
const uuidOf = (label: string) => `8b9a1e2c-0000-4000-8000-${label.padStart(12, '0')}`

describe('readSessionClaims', () => {
  const payload = {
    slot: 3,
    user_uuid: uuidOf('a'),
    session_created_at: 1_700_000_000_000,
    iss: 'gradido',
    aud: 'gradido-backend',
    iat: 1_700_000_000,
    exp: 1_700_001_800,
  }

  test('reads the three claims a session is named by and leaves the token its own', () => {
    expect(readSessionClaims(payload)).toEqual({
      slot: 3,
      userUuid: uuidOf('a'),
      sessionCreatedAt: 1_700_000_000_000,
    })
  })

  // The one that matters: a missing claim must not become slot 0, which is a valid slot
  // and usually holds someone. Architecture.md, Safety net -- absent is not passed.
  test('a missing slot is nothing, not slot zero', () => {
    const { slot, ...withoutSlot } = payload
    expect(slot).toBe(3)
    expect(readSessionClaims(withoutSlot)).toBeUndefined()
  })

  test('rejects a slot that is not one', () => {
    for (const slot of [null, '3', 3.5, -1, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(readSessionClaims({ ...payload, slot })).toBeUndefined()
    }
  })

  test('rejects a user that is not a uuid', () => {
    for (const user_uuid of [undefined, '', 'user-a', 42, `${uuidOf('a')} `]) {
      expect(readSessionClaims({ ...payload, user_uuid })).toBeUndefined()
    }
  })

  test('rejects a creation time that is not unix milliseconds', () => {
    for (const session_created_at of [undefined, null, '1700000000000', -1, 1.5]) {
      expect(readSessionClaims({ ...payload, session_created_at })).toBeUndefined()
    }
  })

  test('rejects what is not a payload at all', () => {
    for (const nonsense of [undefined, null, 'a string', 42, [payload]]) {
      expect(readSessionClaims(nonsense)).toBeUndefined()
    }
  })

  test('spells the claims back out the way a token carries them', () => {
    const claims = readSessionClaims(payload) as SessionClaims
    const spelled = writeSessionClaims(claims)

    expect(spelled).toEqual({
      slot: 3,
      user_uuid: uuidOf('a'),
      session_created_at: 1_700_000_000_000,
    })
    expect(readSessionClaims(spelled)).toEqual(claims)
  })
})
