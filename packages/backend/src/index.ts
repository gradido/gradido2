import { databaseErrorMessage, waitForDatabase } from '@gradido/backend-core'
import { Logger, setupGracefulShutdown } from '@gradido/service-core'
import { Elysia } from 'elysia'
import { AppContext } from './AppContext'
import { CONFIG } from './config'

async function main(): Promise<void> {
  const logger = Logger.create(CONFIG)
  const appContext = await openDatabase(logger)

  /* No routes yet. They are defined in packages/shared so that frontend, admin and
     frontend-core can derive their types from them via Eden Treaty, and mounted here. */
  const app = new Elysia().listen(CONFIG.BACKEND_PORT, () => {
    logger.info(
      {
        cat: 'startup',
        event: 'startup.server.started',
        data: { impl: 'reference', db: appContext.db.kind, port: CONFIG.BACKEND_PORT },
      },
      `backend listening on http://localhost:${CONFIG.BACKEND_PORT}`,
    )
    setupGracefulShutdown(appContext, async () => {
      await app.stop()
    })
  })
}

/**
 * Opening the database and waiting for it to answer are two failures with one outcome, so
 * they are reported as one line: a database that will not come ends the process here,
 * where the reason is still visible, instead of turning every request into a 500.
 */
async function openDatabase(logger: Logger): Promise<AppContext> {
  try {
    const appContext = new AppContext(logger)
    await waitForDatabase(appContext.db, logger)
    return appContext
  } catch (error) {
    logger.fatal(
      { cat: 'startup', event: 'startup.database.failed', data: { db: CONFIG.DB_TYPE } },
      `cannot reach the database: ${databaseErrorMessage(error)}`,
    )
    logger.flush()
    process.exit(1)
  }
}

main().catch((error) => {
  // biome-ignore lint/suspicious/noConsole: startup can fail before there is a logger
  console.error(error)
  process.exit(1)
})
