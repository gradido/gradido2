import { describe, expect, test } from 'bun:test'
import { Logger } from '@gradido/service-core'
import type { DatabaseConnection } from './connect'
import { databaseErrorMessage, isDatabaseStartingUp, waitForDatabase } from './wait'

const quietLogger = Logger.create({ LOG_LEVEL: 'fatal', LOG_FILE: '', NODE_ENV: 'test' })

/** A connection whose probe fails a given number of times before it answers. */
const connectionThatArrivesAfter = (failures: number, error: unknown) => {
  let probes = 0
  const connection = {
    kind: 'postgresql',
    drizzle: {} as never,
    probe: async () => {
      probes++
      if (probes <= failures) {
        throw error
      }
    },
    close: async () => {
      /* Nothing was opened. */
    },
  } as DatabaseConnection
  return { connection, probes: () => probes }
}

/**
 * What actually arrives at the retry: drizzle's wrapper around the driver's error. The
 * shapes are the ones bun's SQL driver produced against a real PostgreSQL — a refused
 * connection carries no SQLSTATE, a rejected one carries it in `errno`.
 */
const wrapped = (cause: unknown) =>
  Object.assign(new Error('Failed query: select 1\nparams: '), { cause })

const driverError = (message: string, code: string, errno?: string) =>
  Object.assign(new Error(message), { name: 'PostgresError', code, errno })

const refused = () => wrapped(driverError('Connection closed', 'ERR_POSTGRES_CONNECTION_CLOSED'))
const wrongPassword = () =>
  wrapped(
    driverError(
      'password authentication failed for user "gradido"',
      'ERR_POSTGRES_SERVER_ERROR',
      '28P01',
    ),
  )

describe('isDatabaseStartingUp', () => {
  test('a server that never answered is worth waiting for', () => {
    expect(isDatabaseStartingUp(refused())).toBe(true)
  })

  // The regression this test exists for: the SQLSTATE sits on the cause, so a predicate
  // that only looks at the error it is handed retries a wrong password thirty times.
  test('a server that answered and refused is not', () => {
    expect(isDatabaseStartingUp(wrongPassword())).toBe(false)
  })

  test('the database not existing does not fix itself either', () => {
    expect(
      isDatabaseStartingUp(wrapped(driverError('database "x" does not exist', 'E', '3D000'))),
    ).toBe(false)
  })

  test('a server that is still starting up says so, and that is worth waiting for', () => {
    // 57P03 cannot_connect_now: PostgreSQL accepts the connection while it recovers.
    expect(
      isDatabaseStartingUp(
        wrapped(driverError('the database system is starting up', 'E', '57P03')),
      ),
    ).toBe(true)
  })

  test('an error the driver did not produce is retried rather than swallowed', () => {
    expect(isDatabaseStartingUp(new Error('socket hang up'))).toBe(true)
    expect(isDatabaseStartingUp(undefined)).toBe(true)
  })
})

describe('databaseErrorMessage', () => {
  test('reports what the driver said, not what the wrapper repeated', () => {
    expect(databaseErrorMessage(refused())).toBe(
      'Connection closed (ERR_POSTGRES_CONNECTION_CLOSED)',
    )
  })

  test('keeps a log line a line', () => {
    expect(databaseErrorMessage(wrapped(new Error('two\nlines')))).toBe('two lines')
  })

  test('survives something that is not an error', () => {
    expect(databaseErrorMessage('just a string')).toBe('just a string')
  })
})

describe('waitForDatabase', () => {
  test('waits for a database that arrives late', async () => {
    const { connection, probes } = connectionThatArrivesAfter(2, refused())
    await waitForDatabase(connection, quietLogger, { attempts: 5, delayMs: 1 })
    expect(probes()).toBe(3)
  })

  // What is thrown is what the driver threw, wrapper and all -- the caller renders it
  // with databaseErrorMessage rather than being handed a summary.
  test('gives up with the error the driver threw when it never arrives', async () => {
    const { connection, probes } = connectionThatArrivesAfter(Number.POSITIVE_INFINITY, refused())
    const thrown = await waitForDatabase(connection, quietLogger, {
      attempts: 3,
      delayMs: 1,
    }).catch((error: unknown) => error)

    expect(databaseErrorMessage(thrown)).toContain('Connection closed')
    expect(probes()).toBe(3)
  })

  test('does not wait out a refusal', async () => {
    const { connection, probes } = connectionThatArrivesAfter(
      Number.POSITIVE_INFINITY,
      wrongPassword(),
    )
    const thrown = await waitForDatabase(connection, quietLogger, {
      attempts: 30,
      delayMs: 1,
    }).catch((error: unknown) => error)

    expect(databaseErrorMessage(thrown)).toContain('password authentication failed')
    expect(probes()).toBe(1)
  })
})
