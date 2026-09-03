import { cors } from '@elysiajs/cors'
import {
  connectDatabase,
  databaseErrorMessage,
  runMigrations,
  SchemaMismatchError,
  waitForDatabase,
} from '@gradido/backend-core'
import { Logger, setupGracefulShutdown } from '@gradido/service-core'
import { AppContext } from './AppContext'
import { CONFIG } from './config'
import { createBackendApp, type StaticSite, staticRoutes } from './server'
import { migrateDownCommand, resolveHomeCommunity, SetupError } from './setup'

/**
 * What this process was asked to do.
 *
 * `serve` by default, because that is what starting a server means. Going down is its own
 * command for a reason that is not a rule about servers: a serving start migrates *up* to the
 * version its code needs, so taking the database to N-1 and then serving a build that needs N
 * would undo the step and re-apply it in the same breath. Going down means the next thing
 * started is a different build, and that is a separate act.
 */
const COMMANDS = ['serve', 'migrate-down'] as const

/**
 * What the backend needs from whoever started it and cannot work out for itself.
 *
 * Empty when it is started as its own process — `bun src/index.ts`, where the frontend is a
 * vite dev server on its own port. Filled in by the bundle, which carries the built pages
 * inside the executable and hands them over here; see `packages/bundle`.
 */
export type BackendOptions = {
  /** Frontends this process serves beside its routes. See `server/staticRoutes.ts`. */
  readonly sites?: readonly StaticSite[]
}

/**
 * The backend, from the command line down.
 *
 * A function rather than a file that runs on import, because it has two callers: `index.ts`,
 * which is this package started as a process, and the single binary, which starts one of
 * several services and must be able to name this one without running it. `argv` is what came
 * after the program's own name in either case.
 */
export async function runBackend(
  argv: readonly string[],
  options: BackendOptions = {},
): Promise<void> {
  const logger = Logger.create(CONFIG)
  const command = argv[0] ?? 'serve'
  if (!(COMMANDS as readonly string[]).includes(command)) {
    // biome-ignore lint/suspicious/noConsole: an unusable argument, before anything is open
    console.error(`unknown command "${command}". Use one of: ${COMMANDS.join(', ')}`)
    process.exit(1)
  }

  if (command === 'migrate-down') {
    await runMigrateDown(logger)
    return
  }

  const appContext = await open(logger)
  const sites = options.sites ?? []

  /* The routes are in ./server, one file per domain, and the frontend derives its types
     from them through Eden Treaty without importing anything that runs — see server/app.ts.
     Plugins are mounted onto the app rather than wrapped around it: an Elysia instance
     answers NOT_FOUND itself, so a wrapper would swallow the contracted
     ROUTE_NOT_IMPLEMENTED before the app's error handler ever ran. */
  const app = createBackendApp(appContext)
    .use(corsPlugin())
    /* Last, so a path a route already answers is never looked for among the files. With no
       sites it changes nothing: every path takes the same road to ROUTE_NOT_IMPLEMENTED it
       took before. */
    .use(staticRoutes(sites))
    .listen(CONFIG.BACKEND_PORT, () => {
      logger.info(
        {
          cat: 'startup',
          event: 'startup.server.started',
          data: {
            impl: 'reference',
            db: appContext.db.kind,
            port: CONFIG.BACKEND_PORT,
            sites: sites.map((site) => site.name),
          },
        },
        `backend listening on http://localhost:${CONFIG.BACKEND_PORT}`,
      )
      setupGracefulShutdown(appContext, async () => {
        await app.stop()
      })
    })
}

/**
 * A page served from this machine, whatever port it is on.
 *
 * `localhost`, `127.0.0.1` and `[::1]`, http or https. Only a page actually served from the
 * loopback interface can present one of these as its `Origin` — a site on the internet sends
 * its own origin, and nothing it can do to DNS changes that — so answering them widens
 * nothing that faces outward.
 */
const LOOPBACK_ORIGIN = /^https?:\/\/(?:localhost|127\.0\.0\.1|\[::1\])(?::\d+)?$/u

/**
 * Who may call this server from a browser.
 *
 * A Gradido deployment serves the frontend and the backend from one origin — the frontend is
 * a small mithril bundle, and hosting it apart from the server it talks to buys nothing; the
 * bundled binary does exactly that, out of `staticRoutes` above — so in production the browser
 * has no cross-origin question to ask and these headers are never read. Everything except
 * loopback is therefore refused there rather than wildcarded:
 * `Access-Control-Allow-Origin: *` could not carry a session cookie anyway, but it would let
 * any page on the internet read every unauthenticated answer, and that is surface nobody
 * asked for.
 *
 * **Loopback is answered in every mode**, because a page on this machine is a case that keeps
 * turning up whatever NODE_ENV says: the vite dev server on its own port, a local admin page,
 * something someone is debugging against a production build. It costs nothing to allow, for
 * the reason on `LOOPBACK_ORIGIN`.
 *
 * In development anything is answered, so a phone on the same network can reach the dev
 * server. The origin is reflected rather than wildcarded in both cases — the session cookie
 * makes every call credentialed, and a browser refuses `*` for those.
 */
function corsPlugin() {
  const development = CONFIG.NODE_ENV === 'development'
  return cors({
    origin: development
      ? true
      : (request: Request) => LOOPBACK_ORIGIN.test(request.headers.get('origin') ?? ''),
    credentials: true,
    methods: ['GET', 'POST', 'PUT', 'DELETE'],
  })
}

/**
 * The `migrate-down` command: open the database, take it down one migration, stop.
 *
 * Deliberately not `open()` — that migrates up on the way, which is the contradiction above,
 * and it asks for a home community, which a schema operation has no business needing.
 */
async function runMigrateDown(logger: Logger): Promise<void> {
  const db = connectDatabase(CONFIG)
  try {
    await waitForDatabase(db, logger)
    await migrateDownCommand(db, logger)
  } catch (error) {
    /* Not db.migration.denied — that one is about a schema this build cannot run against at
       all, and carries the migration it diverges at. This is the down command declining, and
       the reason is in the sentence rather than in a field, the way db.connection.failed
       carries the driver's own message. */
    logger.fatal(
      { cat: 'db', event: 'db.migration.refused', data: { db: CONFIG.DB_TYPE } },
      error instanceof Error ? error.message : String(error),
    )
    logger.flush()
    await db.close().catch(() => {
      /* Already exiting; a connection that will not close changes nothing about that. */
    })
    process.exit(1)
  }
  logger.flush()
  await db.close()
}

/**
 * Everything that has to be true before a request can be served, in the order it becomes
 * true: the database answers, its schema is current, and this instance knows which community
 * it is. On an empty database the last step is a conversation with whoever started the
 * process — see `setup/`.
 *
 * All four failures have one outcome, so they are reported as one line: a database that will
 * not come, will not migrate or has no community ends the process here, where the reason is
 * still visible, instead of turning every request into a 500.
 */
async function open(logger: Logger): Promise<AppContext> {
  const db = connectDatabase(CONFIG)
  try {
    await waitForDatabase(db, logger)
    await runMigrations(db, logger)
    const homeCommunity = await resolveHomeCommunity({ db, logger })
    return new AppContext(logger, db, homeCommunity)
  } catch (error) {
    /* Two failures with one outcome but not one cause: a database that will not answer is
       an operator's problem with a service, an instance that cannot be set up is a step
       nobody has taken yet. Whoever reads the log needs to know which. */
    if (error instanceof SchemaMismatchError) {
      /* Already reported as db.migration.denied, with the migration named and what to do
         about it. Saying it again under a heading about reaching the database would only
         make the useful line harder to find. */
      logger.flush()
      await db.close().catch(() => {
        /* Already exiting; a connection that will not close changes nothing about that. */
      })
      process.exit(1)
    }
    if (error instanceof SetupError) {
      logger.fatal(
        { cat: 'startup', event: 'startup.setup.failed', data: { reason: error.reason } },
        `cannot start: ${error.message}`,
      )
    } else {
      logger.fatal(
        { cat: 'startup', event: 'startup.database.failed', data: { db: CONFIG.DB_TYPE } },
        `cannot reach the database: ${databaseErrorMessage(error)}`,
      )
    }
    logger.flush()
    await db.close().catch(() => {
      /* Already failing; a connection that will not close changes nothing about that. */
    })
    process.exit(1)
  }
}
