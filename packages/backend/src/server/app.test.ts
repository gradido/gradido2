import { afterEach, describe, expect, mock, spyOn, test } from 'bun:test'
import { DatabaseGate } from '@gradido/backend-core'
import { CriticalError, Logger, type LogLine } from '@gradido/service-core'
import { ErrorCode } from '@gradido/shared/errors'
import type { AppContext } from '../AppContext'
import { createBackendApp } from './app'

/**
 * SERVICE_BUSY end to end on the reference path: a request that gets no place at the database is
 * answered 503, with the wait in `Retry-After` and the contracted body -- the same answer
 * `fast-servers` gives from `sc_http_reply_busy`. The database is never reached, which is the
 * point: the gate refuses before anything runs, so this needs no database at all.
 */
describe('a request the database has no place for', () => {
  test('is answered 503 SERVICE_BUSY with Retry-After', async () => {
    const gate = new DatabaseGate(1, 20)
    // biome-ignore lint/complexity/noVoid: the one place, taken for good — this promise is meant never to settle, so it is the one call that must not be awaited or caught
    void gate.run(() => new Promise<never>(() => undefined))
    const context = {
      logger: Logger.create({ LOG_LEVEL: 'fatal', LOG_FILE: '', NODE_ENV: 'test' }),
      gate,
    } as unknown as AppContext

    const response = await createBackendApp(context).handle(
      new Request('http://localhost/user/create', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({
          firstName: 'Einhorn',
          lastName: 'Immond',
          email: 'einhorn@gradido.net',
          language: 'de',
        }),
      }),
    )

    expect(response.status).toBe(503)
    expect(response.headers.get('retry-after')).toBe('1')
    expect(await response.json()).toEqual({
      error: {
        code: ErrorCode.ServiceBusy,
        name: 'SERVICE_BUSY',
        message: 'service busy, retry after 1 seconds',
      },
    })
  })
})

/**
 * A critical error on the way through a request: every cause is logged as the fatal event the
 * error carries, the process is sent SIGTERM, and the request is still answered -- as any
 * other that failed, saying nothing about why.
 */
describe('a request that meets a critical error', () => {
  afterEach(() => {
    mock.restore()
  })

  test('is answered 500, logged fatal once per cause, and stops the process', async () => {
    const kill = spyOn(process, 'kill').mockImplementation(() => true)
    const line: LogLine = {
      cat: 'db',
      event: 'db.transaction.failed',
      data: { db: 'sqlite', step: 'rollback' },
    }
    const critical = new CriticalError(line, 'the rollback failed', [
      new Error('the work failed'),
      new Error('disk I/O error'),
    ])
    const fatal = mock<(line: LogLine, msg: string) => void>(() => undefined)
    const error = mock<(line: LogLine, msg: string) => void>(() => undefined)
    const context = {
      logger: { fatal, error, flush: () => undefined },
      gate: { run: () => Promise.reject(critical) },
    } as unknown as AppContext

    const response = await createBackendApp(context).handle(
      new Request('http://localhost/user/create', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({
          firstName: 'Einhorn',
          lastName: 'Immond',
          email: 'einhorn@gradido.net',
          language: 'de',
        }),
      }),
    )

    expect(response.status).toBe(500)
    expect(fatal.mock.calls).toEqual([
      [line, 'the rollback failed: the work failed'],
      [line, 'the rollback failed: disk I/O error'],
    ])
    expect(error.mock.calls.map(([logged]) => logged.event)).toEqual(['http.request.failed'])
    expect(kill).toHaveBeenCalledWith(process.pid, 'SIGTERM')
  })
})
