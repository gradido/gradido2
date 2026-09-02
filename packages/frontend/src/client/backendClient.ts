import { treaty } from '@elysiajs/eden'
import type { UserRoutes } from '@gradido/backend/server'
import { CONFIG } from '../config'

/* Everything a client of ours sends. The session will travel in a cookie: same-origin in a
   deployment, where the backend serves this bundle too, and cross-origin only in
   development, which is the one case the backend sends CORS headers for. */
const options = { fetch: { credentials: 'include' } } as const

/**
 * The `user` domain of the backend, as a typed object.
 *
 * `UserRoutes` is the type of the Elysia instance in
 * `packages/backend/src/server/userRoutes.ts` — the very definition the server mounts — so
 * `userApi.create.post(...)` is checked against that route's own request and response
 * schemas. A path that does not exist, a field with the wrong name or a response read as
 * the wrong shape is a compile error here. That is the whole point of Eden Treaty, and it
 * is why the routes may stay in the backend where they belong.
 *
 * **Nothing of the server crosses into the bundle.** `UserRoutes` is a type, `import type`
 * is erased before the bundler looks at the module, and what ships is Eden's wrapper around
 * `fetch`. The one rule that keeps it that way: never import a *value* from
 * `@gradido/backend` here. Error codes and schemas the page needs at runtime live in
 * `@gradido/shared`, which is written to be loadable in a browser.
 *
 * Bound per domain rather than to the whole application, so this client's type graph holds
 * the user routes and not every schema the backend will ever declare. The route carries
 * `prefix: '/user'`, which is what the trailing `.user` selects — once, here, instead of in
 * every call.
 */
export const userApi = treaty<UserRoutes>(CONFIG.API_BASE_URL, options).user
