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

  /** Where the backend lives. Same origin by default, which is how it is deployed. */
  API_BASE_URL: v.optional(v.pipe(v.string(), v.url()), 'http://localhost:4000/api'),

  /** Sub-path the app is served under, e.g. `/wallet`. Empty means the domain root. */
  BASE_PATH: v.optional(v.string(), ''),

  /** Shown as the community's name on the login page. Per-deployment, not per-build. */
  COMMUNITY_NAME: v.optional(v.string(), 'Gradido'),

  /** Public website, linked from the auth pages. The locale is appended. */
  WEBSITE_URL: v.optional(v.pipe(v.string(), v.url()), 'https://gradido.net'),

  DEV_SERVER_PORT: port(3000),
})

export type Config = v.InferOutput<typeof configSchema>
