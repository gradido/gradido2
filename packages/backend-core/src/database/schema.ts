import { envPort } from 'service-core'
import * as v from 'valibot'

/**
 * Which database is used is a startup decision, not a build-time one: PostgreSQL is the
 * reference and the default for server mode, SQLite is what makes a small community's
 * installation a download-and-start affair. See Architecture.md, DB.
 *
 * The variables live here rather than in each service because the backend and the
 * federation server talk to the same database and must name it the same way.
 */
export const dbSchema = v.object({
  DB_TYPE: v.optional(v.picklist(['postgresql', 'sqlite']), 'postgresql'),
  /* PostgreSQL only. */
  DB_HOST: v.optional(v.string(), 'localhost'),
  DB_PORT: envPort('5432'),
  DB_USER: v.optional(v.string(), 'gradido'),
  DB_PASSWORD: v.optional(
    v.pipe(
      v.string(),
      v.custom(
        (input: unknown): boolean => process.env.NODE_ENV !== 'production' || input !== '',
        'an empty database password is not acceptable in production',
      ),
    ),
    '',
  ),
  DB_DATABASE: v.optional(v.string(), 'gradido_community'),
  /* SQLite only. Relative paths are resolved against the working directory. */
  DB_FILE: v.optional(v.string(), './gradido_community.sqlite'),
})

/** What connectDatabase needs from the environment. */
export type DatabaseEnv = v.InferOutput<typeof dbSchema>
