import { Logger } from '../logging'
import { CriticalError } from './CriticalError'
import { setupGracefulShutdown, stopAfterCriticalError } from './shutdown'

/**
 * A service that meets a critical error a moment after it started. Run as its own process by
 * `shutdown.test.ts`, which reads what it logged and how it exited.
 */
const logger = Logger.create({ LOG_LEVEL: 'info', LOG_FILE: '', NODE_ENV: 'production' })
let closed = false

setupGracefulShutdown(
  {
    logger,
    close: async () => {
      closed = true
    },
  },
  () => undefined,
)

process.on('exit', () => {
  process.stdout.write(`${JSON.stringify({ fixture: 'exit', closed })}\n`)
})

/* Keeps the process alive, so that only the signal can end it. */
setInterval(() => undefined, 1000)

stopAfterCriticalError(
  logger,
  new CriticalError(
    { cat: 'db', event: 'db.transaction.failed', data: { db: 'sqlite', step: 'rollback' } },
    'the rollback failed and the transaction is still open',
    [new Error('the work failed'), new Error('disk I/O error')],
  ),
)
