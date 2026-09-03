import { runBackend } from './main'

/**
 * The backend as its own process: `bun src/index.ts [serve|migrate-down]`.
 *
 * Everything it does is in `main.ts`, because the single binary starts the same backend
 * without being this file — it names `runBackend` among the services it can start, and a
 * module that runs on import cannot be named. See `packages/bundle`.
 */
runBackend(process.argv.slice(2)).catch((error) => {
  // biome-ignore lint/suspicious/noConsole: startup can fail before there is a logger
  console.error(error)
  process.exit(1)
})
