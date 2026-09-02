import {
  DATABASE_PASSWORD_MESSAGE,
  databaseConfigSchema,
  isDatabasePasswordAcceptable,
} from '@gradido/backend-core'
import { portSchema, runtimeConfigSchema } from '@gradido/service-core'
import * as v from 'valibot'

const backendConfigSchema = v.object({
  ...runtimeConfigSchema.entries,
  ...databaseConfigSchema.entries,
  BACKEND_PORT: v.optional(portSchema, '4000'),
  /**
   * Confirms a down migration on a release — see `setup/migrateDownCommand.ts`.
   *
   * The value is the migration the database should end at — one lower than where it is —
   * or `0` for an empty database. At `0002_users` that is `DB_MIGRATE_DOWN=0001_communities`.
   *
   * A target and not a yes: a boolean left behind in an env file stays true and permits the
   * next down run too, while a target stops being one lower the moment it is reached. It
   * disarms itself, and it says which state was meant, which for a one-step operation is the
   * whole confirmation.
   */
  DB_MIGRATE_DOWN: v.optional(v.string(), ''),
})

type BackendConfig = v.InferOutput<typeof backendConfigSchema>

export const configSchema = v.pipe(
  backendConfigSchema,
  /* Forwarded onto DB_PASSWORD so the failure names the variable somebody has to set:
     grabEnvAndCheckBySchema prints the first segment of the issue path. The parameter is
     annotated with the whole config on purpose — a check typed by the narrower shape the
     rule reads would make the pipe answer with that shape and drop every other variable. */
  v.forward(
    v.check(
      (config: BackendConfig) => isDatabasePasswordAcceptable(config),
      DATABASE_PASSWORD_MESSAGE,
    ),
    ['DB_PASSWORD'],
  ),
)

export type Config = v.InferOutput<typeof configSchema>
