import { describe, expect, test } from 'bun:test'
import { DatabaseBusy, DatabaseGate } from './gate'

/**
 * The gate in front of the database: the same three properties `test_db_exec.cpp` asserts on the
 * fast path -- a bounded queue refused at once, a wait that ends, and a place given back that
 * goes to whoever waited longest.
 */

/** Work that runs until the test lets it finish. */
function held(): { promise: Promise<void>; release: () => void } {
  let release: () => void = () => undefined
  const promise = new Promise<void>((resolve) => {
    release = resolve
  })
  return { promise, release }
}

/** Work that does nothing and says so. */
const nothing = async (): Promise<undefined> => undefined

describe('DatabaseGate', () => {
  test('runs work at once while a place is free', async () => {
    const gate = new DatabaseGate(2)
    expect(await gate.run(async () => 'done')).toBe('done')
  })

  test('gives the place back when the work throws', async () => {
    const gate = new DatabaseGate(1)
    await expect(
      gate.run(async () => {
        throw new Error('the database refused')
      }),
    ).rejects.toThrow('the database refused')
    expect(await gate.run(async () => 'next')).toBe('next')
  })

  test('hands a place given back to the longest waiter', async () => {
    const gate = new DatabaseGate(1)
    const first = held()
    const order: string[] = []

    const running = gate.run(() => first.promise)
    const a = gate.run(async () => {
      order.push('a')
    })
    const b = gate.run(async () => {
      order.push('b')
    })
    expect(gate.queued).toBe(2)
    first.release()
    await Promise.all([running, a, b])
    expect(order).toEqual(['a', 'b'])
  })

  /* The wait ends, and the work behind it never runs: nothing half done is what makes the 503
     that follows safe to retry. */
  test('refuses work that waited too long, without running it', async () => {
    const gate = new DatabaseGate(1, 30)
    const first = held()
    let ran = false

    const running = gate.run(() => first.promise)
    const started = performance.now()
    await expect(
      gate.run(async () => {
        ran = true
      }),
    ).rejects.toBeInstanceOf(DatabaseBusy)
    expect(performance.now() - started).toBeGreaterThanOrEqual(25)
    expect(ran).toBe(false)
    expect(gate.queued).toBe(0)
    first.release()
    await running
  })

  test('refuses at once when the queue is already full', async () => {
    const gate = new DatabaseGate(1, 5000, 2)
    const first = held()

    const running = gate.run(() => first.promise)
    const queued = [gate.run(nothing), gate.run(nothing)]
    const started = performance.now()
    const refused = await gate.run(nothing).catch((error: unknown) => error)
    expect(refused).toBeInstanceOf(DatabaseBusy)
    expect((refused as DatabaseBusy).retryAfter).toBe(1)
    expect(performance.now() - started).toBeLessThan(20)
    first.release()
    await Promise.all([running, ...queued])
  })
})
