import * as v from 'valibot'

/**
 * The error codes a route can answer with — `contracts/errors/*.json`.
 *
 * In `@gradido/shared` rather than beside the routes, and that is load-bearing rather than
 * tidy: the frontend needs these *values* to decide what to do with a failure, while the
 * routes live in `@gradido/backend`, which reaches Elysia and the database. An earlier
 * layout kept them next to the route definitions, and a single value import pulled the whole
 * HTTP framework and TypeBox into the browser bundle — 213 kB of a server the browser will
 * never run. *Types* from the backend stay free, because `import type` is erased, which is
 * exactly what Eden Treaty needs and all it needs. Same rule as `AGENTS.md`, section 2: do
 * not import what you cannot load.
 *
 * Numbers rather than strings, because the fast path formats the same error from the same
 * template and a C switch on an integer is what both sides can agree on cheaply. A code is
 * permanent: `contracts/AGENTS.md` forbids renumbering, so a value here is never reused for
 * something else and an older frontend keeps understanding what it already understood.
 *
 * The ranges say which contract file a code comes from — 2000–2999 domain, 3000–3999 api —
 * and codes from both meet here because a client does not care which of our files an error
 * was declared in. Only the codes something actually emits are listed: an enum that mirrors
 * a whole file nobody reads drifts from it silently.
 */
export enum ErrorCode {
  /** `errors/domain.json`. Replaces legacy's MutationErrorType.VALIDATION_ERROR. */
  ValidationFailed = 2003,
  /** `errors/api.json`. What a client is told when the log knows more than it should. */
  Unknown = 3001,
  /** `errors/api.json`. This deployment's implementation does not serve that route. */
  RouteNotImplemented = 3008,
}

/**
 * What a failing route answers with.
 *
 * One shape for every error, so a client has one thing to parse and not one per route. The
 * message is formatted from the contract's template and is for a log and a developer; it is
 * English and never translated. A frontend showing it to a member is showing the wrong
 * thing — the code is what a client decides on.
 */
export const errorBodySchema = v.object({
  error: v.object({
    code: v.enum(ErrorCode),
    name: v.string(),
    message: v.string(),
  }),
})

export type ErrorBody = v.InferOutput<typeof errorBodySchema>

/** The name each code carries in the contract, so a response does not have to look it up. */
const ERROR_NAMES: Record<ErrorCode, string> = {
  [ErrorCode.ValidationFailed]: 'VALIDATION_FAILED',
  [ErrorCode.Unknown]: 'UNKNOWN',
  [ErrorCode.RouteNotImplemented]: 'ROUTE_NOT_IMPLEMENTED',
}

/** The HTTP status the contract gives each code, kept beside the code it belongs to. */
const ERROR_STATUS: Record<ErrorCode, number> = {
  [ErrorCode.ValidationFailed]: 400,
  [ErrorCode.Unknown]: 500,
  [ErrorCode.RouteNotImplemented]: 501,
}

export function errorStatus(code: ErrorCode): number {
  return ERROR_STATUS[code]
}

export function errorBody(code: ErrorCode, message: string): ErrorBody {
  return { error: { code, name: ERROR_NAMES[code], message } }
}
