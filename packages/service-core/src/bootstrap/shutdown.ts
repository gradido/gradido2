import { createInterface } from 'node:readline'
import type { ServiceContext } from '..'

/**
 * How long a shutdown may take before the process is killed anyway. A request that hangs
 * must not keep a stopping server alive -- an orchestrator that waits for a clean exit
 * would then send SIGKILL itself, and the exit code would say the process crashed.
 */
const FORCED_EXIT_TIMEOUT_MS = 10_000

interface Role {
  readonly context: ServiceContext
  readonly stop: () => Promise<void> | void
}

/** Every role of this process that registered, in the order they did. */
const roles: Role[] = []
let shuttingDown = false

/**
 * Stops the service on SIGINT and SIGTERM: no new work accepted, what is running finished,
 * the context closed, buffered log lines written, then exit.
 *
 * Called once per role. Several roles in one process -- backend and dht-node -- are stopped
 * together by one handler, and the process exits after the last of them: a handler per role
 * would let the quickest one end the process while another still closes its database.
 *
 * @param stop stops accepting new work and lets what is running finish -- an HTTP server
 *   stops listening, the dht-node leaves the network
 */
export function setupGracefulShutdown(
  context: ServiceContext,
  stop: () => Promise<void> | void,
): void {
  roles.push({ context, stop })
  if (roles.length > 1) {
    return
  }

  for (const signal of ['SIGINT', 'SIGTERM'] as NodeJS.Signals[]) {
    process.on(signal, async () => {
      /* A second Ctrl+C while the first one is still being handled would close the
         database twice. It means "hurry up", not "do it again". */
      if (shuttingDown) {
        return
      }
      shuttingDown = true
      await gracefulShutdown(signal)
    })
  }

  if (process.platform === 'win32') {
    /* Windows has no signals: without a readline interface on stdin, Ctrl+C never reaches
       the process handler and the server dies without closing anything. */
    const rl = createInterface({ input: process.stdin, output: process.stdout })
    rl.on('SIGINT', () => {
      process.emit('SIGINT', 'SIGINT')
    })
  }
}

async function gracefulShutdown(signal: NodeJS.Signals): Promise<void> {
  const loggers = roles.map((role) => role.context.logger)

  const forcedExit = setTimeout(() => {
    for (const logger of loggers) {
      logger.fatal(
        { cat: 'startup', event: 'startup.shutdown.failed', data: { signal, reason: 'timeout' } },
        'shutdown takes too long, exiting anyway',
      )
      logger.flush()
    }
    process.exit(1)
  }, FORCED_EXIT_TIMEOUT_MS)

  const outcomes = await Promise.allSettled(
    roles.map(async (role) => {
      await role.stop()
      await role.context.close()
    }),
  )

  let failed = false
  outcomes.forEach((outcome, index) => {
    const logger = loggers[index]
    if (logger === undefined) {
      return
    }
    if (outcome.status === 'fulfilled') {
      logger.info(
        { cat: 'startup', event: 'startup.server.stopped', data: { signal } },
        'service stopped',
      )
    } else {
      failed = true
      logger.fatal(
        { cat: 'startup', event: 'startup.shutdown.failed', data: { signal, reason: 'error' } },
        `shutdown failed: ${String(outcome.reason)}`,
      )
    }
    logger.flush()
  })

  clearTimeout(forcedExit)
  process.exit(failed ? 1 : 0)
}
