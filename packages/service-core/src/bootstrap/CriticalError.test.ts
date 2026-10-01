import { afterEach, describe, expect, mock, spyOn, test } from 'bun:test'
import type { Logger, LogLine } from '../logging'
import { CriticalError } from './CriticalError'
import { stopAfterCriticalError } from './shutdown'

const line = {
  cat: 'db',
  event: 'db.transaction.failed',
  data: { db: 'sqlite', step: 'rollback' },
} as const

describe('CriticalError', () => {
  test('holds every error it was given', () => {
    const first = new Error('first')
    const second = new Error('second')
    const error = new CriticalError(line, 'cannot continue', [first, second])

    expect(error).toBeInstanceOf(AggregateError)
    expect(error.errors).toEqual([first, second])
    expect(error.reasons()).toEqual(['first', 'second'])
  })

  test('lets the deepest cause speak, not the wrapper that quotes the statement', () => {
    const wrapper = new Error('Failed query: insert into users values ($1)\nparams: secret', {
      cause: new Error('disk I/O error'),
    })

    expect(new CriticalError(line, 'cannot continue', [wrapper]).reasons()).toEqual([
      'disk I/O error',
    ])
  })

  test('says something for a thing that is no Error', () => {
    expect(new CriticalError(line, 'cannot continue', ['just a string']).reasons()).toEqual([
      'just a string',
    ])
  })
})

describe('stopAfterCriticalError', () => {
  afterEach(() => {
    mock.restore()
  })

  test('writes one fatal line per error, flushes, then sends itself SIGTERM', () => {
    const order: string[] = []
    const kill = spyOn(process, 'kill').mockImplementation(() => {
      order.push('kill')
      return true
    })
    const fatal = mock<(line: LogLine, msg: string) => void>(() => {
      order.push('fatal')
    })
    const flush = mock(() => {
      order.push('flush')
    })
    const logger = { fatal, flush } as unknown as Logger

    stopAfterCriticalError(
      logger,
      new CriticalError(line, 'cannot continue', [new Error('first'), new Error('second')]),
    )

    expect(fatal.mock.calls).toEqual([
      [line, 'cannot continue: first'],
      [line, 'cannot continue: second'],
    ])
    expect(order).toEqual(['fatal', 'fatal', 'flush', 'kill'])
    expect(kill).toHaveBeenCalledWith(process.pid, 'SIGTERM')
  })
})
