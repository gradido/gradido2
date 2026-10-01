import { afterEach, beforeEach, describe, expect, test } from 'bun:test'
import { mkdtempSync, readFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import pino from 'pino'
import { Logger } from './logger'

/**
 * `flush` has to have written when it returns, because what follows it is `process.exit`.
 */
describe('Logger.flush', () => {
  let directory: string

  beforeEach(() => {
    directory = mkdtempSync(join(tmpdir(), 'gradido-logger-'))
  })

  afterEach(() => {
    rmSync(directory, { recursive: true, force: true })
  })

  const written = (path: string) =>
    readFileSync(path, 'utf8')
      .split('\n')
      .filter((line) => line !== '')
      .map((line) => (JSON.parse(line) as { data: { n: number } }).data.n)

  test('has written every line by the time it returns', async () => {
    const path = join(directory, 'flushed.log')
    const logger = Logger.create({ LOG_LEVEL: 'fatal', LOG_FILE: path, NODE_ENV: 'production' })
    /* The file is opened asynchronously; this test is about a destination that is open. */
    await Bun.sleep(50)

    for (let n = 1; n <= 5; n++) {
      logger.fatal({ cat: 'startup', event: 'startup.config.failed', data: { n } }, 'a line')
    }
    logger.flush()

    expect(written(path)).toEqual([1, 2, 3, 4, 5])
  })

  test('reaches the same destinations through a child logger', async () => {
    const path = join(directory, 'child.log')
    const logger = Logger.create({ LOG_LEVEL: 'fatal', LOG_FILE: path, NODE_ENV: 'production' })
    await Bun.sleep(50)

    const child = logger.child({ req: 'r1' })
    child.fatal({ cat: 'startup', event: 'startup.config.failed', data: { n: 1 } }, 'a line')
    child.flush()

    expect(written(path)).toEqual([1])
  })

  test('does not throw for a file that is still being opened, and loses nothing', async () => {
    const path = join(directory, 'not-open-yet.log')
    const logger = Logger.create({ LOG_LEVEL: 'fatal', LOG_FILE: path, NODE_ENV: 'production' })

    logger.fatal({ cat: 'startup', event: 'startup.config.failed', data: { n: 1 } }, 'a line')

    expect(() => logger.flush()).not.toThrow()
    /* Held until the file is open, and written then. Waiting for that also keeps the open from
       finishing into a directory the cleanup has already removed. */
    await Bun.sleep(50)
    expect(written(path)).toEqual([1])
  })

  /* Why `flush` goes to the destinations itself instead of calling pino's. */
  test('pino.flush on a multistream has nothing to call, and calls back at once', () => {
    const streams = pino.multistream([
      { level: 'fatal', stream: pino.destination({ dest: 1, sync: false }) },
    ])
    let calledBack = false

    pino({ base: null }, streams).flush(() => {
      calledBack = true
    })

    expect((streams as { flush?: unknown }).flush).toBeUndefined()
    expect(calledBack).toBe(true)
  })
})
