import { describe, expect, test } from 'bun:test'
import * as v from 'valibot'
import { configFailureOf } from './grabEnvAndCheckSchema'
import { SecretUnreadable } from './secret'

const schema = v.object({
  BACKEND_PORT: v.pipe(v.string(), v.regex(/^[0-9]+$/u, 'not a port')),
  DB_PASSWORD: v.pipe(v.string(), v.minLength(20, 'too short')),
})

const thrownBy = (env: Record<string, unknown>): unknown => {
  try {
    v.parse(schema, env)
  } catch (error) {
    return error
  }
  throw new Error('the configuration was accepted')
}

describe('configFailureOf', () => {
  test('names the variable a schema refused, and quotes what it was given', () => {
    expect(
      configFailureOf(thrownBy({ BACKEND_PORT: 'notaport', DB_PASSWORD: 'x'.repeat(20) })),
    ).toEqual({
      variable: 'BACKEND_PORT',
      reason: 'invalid',
      message: 'BACKEND_PORT: not a port, received: "notaport"',
    })
  })

  test('does not quote a secret it refused', () => {
    const failure = configFailureOf(thrownBy({ BACKEND_PORT: '1', DB_PASSWORD: 'hunter2' }))

    expect(failure).toEqual({
      variable: 'DB_PASSWORD',
      reason: 'invalid',
      message: 'DB_PASSWORD: too short',
    })
    expect(JSON.stringify(failure)).not.toContain('hunter2')
  })

  test('says unreadable for a secret whose named source cannot be read', () => {
    expect(
      configFailureOf(new SecretUnreadable('EMAIL_PASSWORD', 'EMAIL_PASSWORD_FILE names …')),
    ).toEqual({
      variable: 'EMAIL_PASSWORD',
      reason: 'unreadable',
      message: 'EMAIL_PASSWORD_FILE names …',
    })
  })

  test('names no variable for a failure that has none', () => {
    expect(configFailureOf(new Error('something else'))).toEqual({
      variable: '',
      reason: 'invalid',
      message: 'something else',
    })
  })
})
