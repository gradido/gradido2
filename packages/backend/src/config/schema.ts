import {
  DATABASE_PASSWORD_MESSAGE,
  databaseConfigSchema,
  isDatabasePasswordAcceptable,
} from '@gradido/backend-core'
import {
  EMAIL_HOST_MESSAGE,
  EMAIL_SENDER_MESSAGE,
  emailConfigSchema,
  hasEmailHost,
  hasEmailSender,
  portSchema,
  runtimeConfigSchema,
} from '@gradido/service-core'
import * as v from 'valibot'

const backendConfigSchema = v.object({
  ...runtimeConfigSchema.entries,
  ...databaseConfigSchema.entries,
  ...emailConfigSchema.entries,
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
  /* Each rule is forwarded onto the variable somebody has to set, because that is the one
     `grabEnvAndCheckBySchema` prints: it names the first segment of the issue path. Every
     one of them needs two variables to decide — a password is only wrong for a *postgresql*
     in *production*, a sender is only missing when `EMAIL` is true — so they are checks on
     the whole config rather than pipes on the field. The parameter is annotated with the
     whole config on purpose: a check typed by the narrower shape the rule reads would make
     the pipe answer with that shape and drop every other variable. */
  v.forward(
    v.check(
      (config: BackendConfig) => isDatabasePasswordAcceptable(config),
      DATABASE_PASSWORD_MESSAGE,
    ),
    ['DB_PASSWORD'],
  ),
  v.forward(
    v.check((config: BackendConfig) => hasEmailHost(config), EMAIL_HOST_MESSAGE),
    ['EMAIL_SMTP_HOST'],
  ),
  v.forward(
    v.check((config: BackendConfig) => hasEmailSender(config), EMAIL_SENDER_MESSAGE),
    ['EMAIL_SENDER'],
  ),
)

export type Config = v.InferOutput<typeof configSchema>
