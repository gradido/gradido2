import { describe, expect, test } from 'bun:test'
import { DatabaseGate } from '@gradido/backend-core'
import { Logger } from '@gradido/service-core'
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
