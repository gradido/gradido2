import { createInterface } from 'node:readline'
import type { ServiceContext } from '../ServiceContext'

/**
 * How long a shutdown may take before the process is killed anyway. A request that hangs
 * must not keep a stopping server alive -- an orchestrator that waits for a clean exit
 * would then send SIGKILL itself, and the exit code would say the process crashed.
 */
const FORCED_EXIT_TIMEOUT_MS = 10_000

/**
 * Stops the service on SIGINT and SIGTERM: no new work accepted, what is running finished,
 * the context closed, buffered log lines written, then exit.
 *
 * @param stop stops accepting new work and lets what is running finish -- an HTTP server
 *   stops listening, the dht-node leaves the network
 */
export function setupGracefulShutdown(
  context: ServiceContext,
  stop: () => Promise<void> | void,
): void {
  let shuttingDown = false
  const signals: NodeJS.Signals[] = ['SIGINT', 'SIGTERM']

  for (const signal of signals) {
    process.on(signal, async () => {
      /* A second Ctrl+C while the first one is still being handled would close the
         database twice. It means "hurry up", not "do it again". */
      if (shuttingDown) {
        return
      }
      shuttingDown = true
      await gracefulShutdown(context, stop, signal)
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

async function gracefulShutdown(
  context: ServiceContext,
  stop: () => Promise<void> | void,
  signal: NodeJS.Signals,
): Promise<void> {
  const logger = context.logger

  const forcedExit = setTimeout(() => {
    logger.fatal(
      { cat: 'startup', event: 'startup.shutdown.failed', data: { signal, reason: 'timeout' } },
      'shutdown takes too long, exiting anyway',
    )
    logger.flush()
    process.exit(1)
  }, FORCED_EXIT_TIMEOUT_MS)

  try {
    await stop()
    await context.close()
    logger.info(
      { cat: 'startup', event: 'startup.server.stopped', data: { signal } },
      'service stopped',
    )
    logger.flush()
  } catch (error) {
    logger.fatal(
      { cat: 'startup', event: 'startup.shutdown.failed', data: { signal, reason: 'error' } },
      `shutdown failed: ${String(error)}`,
    )
    logger.flush()
    clearTimeout(forcedExit)
    process.exit(1)
  }

  clearTimeout(forcedExit)
  process.exit(0)
}
