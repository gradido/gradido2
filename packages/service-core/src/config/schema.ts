import * as v from 'valibot'

/**
 * A TCP port from the environment, with a default. Environment variables are strings;
 * a port that is not a number is a configuration error, not a runtime surprise.
 */
export const envPort = (defaultValue: string) =>
  v.optional(
    v.pipe(
      v.string(),
      v.transform<string, number>((input: string) => Number.parseInt(input, 10)),
      v.integer(),
      v.minValue(1),
      v.maxValue(65535),
    ),
    defaultValue,
  )

/**
 * The variables every service reads, whatever else it reads. Spread into a service's own
 * schema: `v.object({ ...serviceSchema.entries, BACKEND_PORT: envPort('4000') })`.
 */
export const serviceSchema = v.object({
  LOG_LEVEL: v.optional(v.picklist(['trace', 'debug', 'info', 'warn', 'error', 'fatal']), 'info'),
  /* Empty means stdout only -- the right answer under systemd or docker, which capture it
     themselves. A path additionally writes the same JSON lines to that file. */
  LOG_FILE: v.optional(v.string(), ''),
  NODE_ENV: v.optional(v.picklist(['development', 'production', 'test']), 'development'),
})

/** What a Logger needs from the environment. */
export type ServiceEnv = v.InferOutput<typeof serviceSchema>
