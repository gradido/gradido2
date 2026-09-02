import { ErrorCode, errorBody, errorStatus } from '@gradido/shared/errors'
import { Elysia } from 'elysia'
import type { AppContext } from '../AppContext'
import { userRoutes } from './userRoutes'

/**
 * The backend's HTTP surface: the error contract, then one plugin per domain.
 *
 * This is meant to be the *root* instance: mount plugins onto it (`.use(cors())`) rather
 * than mounting it inside another Elysia. A parent instance answers `NOT_FOUND` itself,
 * before this error handler is ever consulted, and the route-not-implemented answer below
 * would silently stop happening.
 *
 * The chain is not a style choice. Elysia derives the application's type from it, and that
 * type is what the frontend binds Eden Treaty to — broken into statements
 * (`const app = new Elysia(); app.use(...)`), the routes stop appearing in it and the
 * frontend loses every check it has, quietly.
 */
export const createBackendApp = (context: AppContext) =>
  new Elysia({ name: 'gradido.backend' })
    .onError(({ code, error, path, request, set }) => {
      /* A request that did not match the route's schema. The frontend validates against the
         same field schemas first, so anything arriving here is a client that did not — the
         message names the field for whoever is writing that client, and no member ever
         sees it. */
      if (code === 'VALIDATION') {
        const { field, reason } = validationDetail(error)
        set.status = errorStatus(ErrorCode.ValidationFailed)
        return errorBody(ErrorCode.ValidationFailed, `validation failed for ${field}: ${reason}`)
      }

      /* Of the 139 routes in `contracts/server`, nearly all are still unwritten, so an
         unknown path on this server is overwhelmingly a contracted route rather than a typo
         — and the contract requires that case to be answered rather than 404'd, because a
         deployment never forwards to the other implementation. A genuine typo gets the same
         answer, which is the price of not loading the contract at runtime. */
      if (code === 'NOT_FOUND') {
        set.status = errorStatus(ErrorCode.RouteNotImplemented)
        return errorBody(ErrorCode.RouteNotImplemented, `route not implemented: ${path}`)
      }

      /* The shape is contracted: contracts/logging.json fixes http.request.failed at method,
         path and status, with the error code in `err`. The exception's own message goes in
         `msg`, which no test compares and which is the only place it belongs — the client is
         told nothing but UNKNOWN. */
      context.logger.error(
        {
          cat: 'http',
          event: 'http.request.failed',
          err: { code: ErrorCode.Unknown, name: 'UNKNOWN' },
          data: { method: request.method, path, status: errorStatus(ErrorCode.Unknown) },
        },
        `unhandled error while serving ${path}: ${error instanceof Error ? error.message : String(error)}`,
      )
      set.status = errorStatus(ErrorCode.Unknown)
      /* Deliberately says nothing: what went wrong is in the log, where it belongs, and not
         in an answer to whoever caused it. */
      return errorBody(ErrorCode.Unknown, 'unknown error')
    })
    .use(userRoutes(context))

/**
 * The whole application as a type — every domain at once.
 *
 * A client that talks to more than one domain binds this; one that talks to a single domain
 * binds that domain's own type instead, e.g. `UserRoutes`. Either way nothing but the type
 * crosses into the bundle.
 */
export type BackendApp = ReturnType<typeof createBackendApp>

/**
 * Which field a request failed on, and why.
 *
 * `contracts/errors/domain.json` templates VALIDATION_FAILED with both, so both have to come
 * out of whatever the validator threw. Elysia keeps the original issue on `valueError`; its
 * own `all` array has already flattened the path into a string that says `[object Object]`,
 * which is why this reads the issue instead.
 */
function validationDetail(error: unknown): { field: string; reason: string } {
  const issue = (error as { valueError?: { message?: unknown; path?: unknown } }).valueError
  const path = Array.isArray(issue?.path)
    ? issue.path
        .map((segment: unknown) => (segment as { key?: unknown }).key)
        .filter((key: unknown) => typeof key === 'string' || typeof key === 'number')
        .join('.')
    : ''
  return {
    /* No path means the body as a whole was wrong — not JSON, or not an object. */
    field: path === '' ? 'body' : path,
    reason: typeof issue?.message === 'string' ? issue.message : 'invalid',
  }
}
