import { portSchema } from '@gradido/service-core'
import * as v from 'valibot'

/**
 * Which database is used is a startup decision, not a build-time one. See Architecture.md, DB.
 *
 * **SQLite is the default, PostgreSQL is the reference.** Those are two different things: the
 * reference is the database the behaviour is defined against and the one a community with an
 * administrator should run, while the default is what happens when nobody has said anything
 * yet. Defaulting to PostgreSQL meant that an unconfigured start spent thirty seconds trying
 * to reach a server nobody had installed and then exited — which is not the download-and-start
 * promise `Architecture.md` makes. Defaulting to SQLite makes an unconfigured start work:
 * a file appears, the schema is created, the community is asked for, the server serves.
 *
 * The cost is worth naming: a deployment that sets `DB_HOST`, `DB_USER` and `DB_PASSWORD` but
 * forgets `DB_TYPE` now runs on SQLite and says so only in the `db` field of its
 * `startup.server.started` line, instead of failing. It is the one mistake this default makes
 * quieter rather than louder.
 *
 * The variables live here rather than in each service because the backend and the
 * federation server talk to the same database and must name it the same way.
 */
export const databaseConfigSchema = v.object({
  DB_TYPE: v.optional(v.picklist(['postgresql', 'sqlite']), 'sqlite'),
  /* PostgreSQL only. */
  DB_HOST: v.optional(v.string(), 'localhost'),
  DB_PORT: v.optional(portSchema, '5432'),
  DB_USER: v.optional(v.string(), 'gradido'),
  DB_PASSWORD: v.optional(v.string(), ''),
  DB_DATABASE: v.optional(v.string(), 'gradido_community'),
  /* SQLite only. Relative paths are resolved against the working directory. */
  DB_FILE: v.optional(v.string(), './gradido_community.sqlite'),
})

/** What connectDatabase needs from the environment. */
export type DatabaseConfig = v.InferOutput<typeof databaseConfigSchema>

/** What the rule below needs to see. A service's config carries all three by construction. */
export type DatabaseCheckable = {
  DB_TYPE: DatabaseConfig['DB_TYPE']
  DB_PASSWORD: string
  NODE_ENV: string
}

export const DATABASE_PASSWORD_MESSAGE =
  'an empty database password is not acceptable in production'

/**
 * Whether this configuration may open its database.
 *
 * A rule that needs two variables cannot live on either of them: a check on `DB_PASSWORD`
 * sees a string and nothing else, so it cannot know which database it is the password *for*.
 * SQLite is a file — it has no password, and demanding one made the download-and-start
 * installation of `Architecture.md` refuse to run in production over a value that means
 * nothing there.
 *
 * It is a predicate here rather than a wrapper around `databaseConfigSchema` because services
 * spread that schema's `entries` into their own object, and a `v.pipe` has no `entries` to
 * spread. Each service applies it to its finished schema, where the object type is concrete:
 *
 * ```ts
 * export const configSchema = v.pipe(
 *   v.object({ ...runtimeConfigSchema.entries, ...databaseConfigSchema.entries, … }),
 *   v.forward(v.check(isDatabasePasswordAcceptable, DATABASE_PASSWORD_MESSAGE), ['DB_PASSWORD']),
 * )
 * ```
 *
 * Reading the parsed config rather than `process.env` is what makes it right: `DB_TYPE` and
 * `NODE_ENV` arrive with their defaults already applied, so an unset `DB_TYPE` is the SQLite it
 * will actually be, not an `undefined` the rule has to guess about.
 */
export function isDatabasePasswordAcceptable(config: DatabaseCheckable): boolean {
  return (
    config.DB_TYPE !== 'postgresql' || config.NODE_ENV !== 'production' || config.DB_PASSWORD !== ''
  )
}
