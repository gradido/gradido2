import { describe, expect, test } from 'bun:test'
import { type RetryAttempt, retry, TimeoutError, wait, withTimeout } from './retry'

/** An operation that fails a given number of times before it succeeds. */
const failsTimes = (times: number, error: unknown = new Error('not yet')) => {
  let calls = 0
  return {
    calls: () => calls,
    operation: async () => {
      calls++
      if (calls <= times) {
        throw error
      }
      return 'connected'
    },
  }
}

describe('retry', () => {
  test('does not wait when the first try succeeds', async () => {
    const { operation, calls } = failsTimes(0)
    expect(await retry(operation, { attempts: 5, delayMs: 10_000 })).toBe('connected')
    expect(calls()).toBe(1)
  })

  test('keeps trying while the thing is still starting up', async () => {
    const { operation, calls } = failsTimes(2)
    expect(await retry(operation, { attempts: 5, delayMs: 1 })).toBe('connected')
    expect(calls()).toBe(3)
  })

  test('throws the error it actually failed with, not a summary of the attempts', async () => {
    const cause = new Error('connection refused')
    const { operation, calls } = failsTimes(Number.POSITIVE_INFINITY, cause)
    await expect(retry(operation, { attempts: 3, delayMs: 1 })).rejects.toThrow(cause)
    expect(calls()).toBe(3)
  })

  test('gives up at once on a failure that will not fix itself', async () => {
    const wrongPassword = new Error('password authentication failed')
    const { operation, calls } = failsTimes(Number.POSITIVE_INFINITY, wrongPassword)
    await expect(
      retry(operation, {
        attempts: 30,
        delayMs: 1,
        shouldRetry: (error) => error !== wrongPassword,
      }),
    ).rejects.toThrow(wrongPassword)
    expect(calls()).toBe(1)
  })

  test('reports every wait and nothing after the last try', async () => {
    const seen: RetryAttempt[] = []
    const { operation } = failsTimes(Number.POSITIVE_INFINITY)
    await retry(operation, {
      attempts: 3,
      delayMs: 1,
      onRetry: (attempt) => seen.push(attempt),
    }).catch(() => undefined)

    expect(seen.map((attempt) => attempt.attempt)).toEqual([1, 2])
    expect(seen[0].attempts).toBe(3)
  })

  test('a try that hangs is abandoned rather than waited out', async () => {
    let started = 0
    const hangs = async () => {
      started++
      await wait(10_000)
      return 'never'
    }
    await expect(retry(hangs, { attempts: 2, delayMs: 1, timeoutMs: 5 })).rejects.toThrow(
      TimeoutError,
    )
    expect(started).toBe(2)
  })

  test('refuses an attempt count that can never run', async () => {
    await expect(retry(async () => 'x', { attempts: 0, delayMs: 1 })).rejects.toThrow(RangeError)
  })
})

describe('withTimeout', () => {
  test('passes the result through when the operation is in time', async () => {
    expect(await withTimeout(async () => 'quick', 1000)).toBe('quick')
  })

  test('names what timed out', async () => {
    await expect(
      withTimeout(() => wait(1000).then(() => 'slow'), 5, 'database probe'),
    ).rejects.toThrow('database probe timed out after 5ms')
  })

  test('a synchronous throw leaves through the same door as a rejection', async () => {
    await expect(
      withTimeout(() => {
        throw new Error('thrown, not rejected')
      }, 1000),
    ).rejects.toThrow('thrown, not rejected')
  })

  // The abandoned operation still fails, later, with nobody listening. Unhandled, that
  // ends the process -- which is why withTimeout swallows it.
  test('a late failure from an abandoned operation does not reach the process', async () => {
    await expect(
      withTimeout(() => wait(5).then(() => Promise.reject(new Error('late'))), 1),
    ).rejects.toThrow(TimeoutError)
    /* Long enough for the abandoned operation to fail with nobody listening. */
    await wait(30)
  })
})
