import * as v from 'valibot'

export enum NodeEnvironmentType {
  Development = 'development',
  Production = 'production',
  /** What `bun test` sets. Behaves like development — only `production` differs. */
  Test = 'test',
}

const port = (fallback: number) =>
  v.optional(
    v.config(
      v.pipe(
        v.string(),
        v.transform(Number),
        v.number(),
        v.integer(),
        v.minValue(1024),
        v.maxValue(65535),
      ),
      { message: 'must be a port between 1024 and 65535' },
    ),
    String(fallback),
  )

export const configSchema = v.object({
  NODE_ENV: v.optional(
    v.pipe(v.string(), v.enum(NodeEnvironmentType)),
    NodeEnvironmentType.Development,
  ),

  /**
   * Where the backend lives. The routes of `contracts/server/backend` are mounted at the
   * root, so this is an origin and nothing more — no `/api` prefix; that one belongs to
   * the federation server, which mounts at `/api/{apiVersion}`.
   *
   * **Empty means the origin this page was served from**, and that is what a deployment
   * uses: the bundled binary hands out this app itself, so the backend is by definition
   * wherever the page came from, and a URL baked in at build time would be a hostname the
   * build would have to know. `scripts/bundle.ts` therefore builds the frontend with this
   * empty unless it is told otherwise.
   *
   * The default is the development one — vite serves the app on a port of its own, so there
   * the backend is somewhere else and has to be named. A deployment overrides it; a checkout
   * without an `.env` still works.
   *
   * The trailing slash is dropped so the two halves of `API_BASE_URL + '/user/create'`
   * cannot both bring one.
   */
  API_BASE_URL: v.optional(
    v.config(
      v.union([
        v.literal(''),
        v.pipe(
          v.string(),
          v.url(),
          v.transform((url) => url.replace(/\/+$/u, '')),
        ),
      ]),
      { message: 'must be a URL, or empty for the origin the page is served from' },
    ),
    'http://localhost:4000',
  ),

  /** Sub-path the app is served under, e.g. `/wallet`. Empty means the domain root. */
  BASE_PATH: v.optional(v.string(), ''),

  /** Shown as the community's name on the login page. Per-deployment, not per-build. */
  COMMUNITY_NAME: v.optional(v.string(), 'Gradido'),

  /** Public website, linked from the auth pages. The locale is appended. */
  WEBSITE_URL: v.optional(v.pipe(v.string(), v.url()), 'https://gradido.net'),

  DEV_SERVER_PORT: port(3000),
})

export type Config = v.InferOutput<typeof configSchema>
