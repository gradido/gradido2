import { describe, expect, test } from 'bun:test'
import { join } from 'node:path'

/**
 * The whole road, in a real process: a critical error is logged, the process sends itself
 * SIGTERM, the registered role is stopped and closed, and the exit code is 1.
 */
describe('a service that meets a critical error', () => {
  test.skipIf(process.platform === 'win32')(
    'logs every cause, stops like on SIGTERM, and exits with 1',
    async () => {
      const child = Bun.spawn(['bun', join(import.meta.dir, 'criticalStop.fixture.ts')], {
        stdout: 'pipe',
        stderr: 'pipe',
      })
      const [out, exitCode] = await Promise.all([new Response(child.stdout).text(), child.exited])
      const lines = out
        .split('\n')
        .filter((text) => text.startsWith('{'))
        .map((text) => JSON.parse(text) as Record<string, unknown>)

      expect(exitCode).toBe(1)
      expect(lines.map((line) => line.event ?? line.fixture)).toEqual([
        'db.transaction.failed',
        'db.transaction.failed',
        'startup.server.stopped',
        'exit',
      ])
      expect(lines[0]).toMatchObject({
        level: 60,
        cat: 'db',
        data: { db: 'sqlite', step: 'rollback' },
        msg: 'the rollback failed and the transaction is still open: the work failed',
      })
      expect(lines[1]).toMatchObject({ level: 60, msg: expect.stringContaining('disk I/O error') })
      expect(lines[2]).toMatchObject({ level: 30, data: { signal: 'SIGTERM' } })
      expect(lines[3]).toEqual({ fixture: 'exit', closed: true })
    },
  )
})
