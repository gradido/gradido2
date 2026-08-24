import * as v from 'valibot'

/**
 * Parses process.env against a schema and exits if it does not fit.
 *
 * A misconfigured server must not start half-working: a missing port or an unreadable
 * database name is discovered here, at boot, with the offending variable named, rather
 * than on the first request that happens to need it.
 */
export function grabEnvAndCheckBySchema<
  const TSchema extends v.BaseSchema<unknown, unknown, v.BaseIssue<unknown>>,
>(schema: TSchema): v.InferOutput<TSchema> {
  try {
    return v.parse(schema, process.env)
  } catch (error) {
    if (error instanceof v.ValiError) {
      const issue = error.issues[0]
      // biome-ignore lint/suspicious/noConsole: the config must be read before a logger can exist
      console.error(
        `${String(issue.path?.[0]?.key)}: ${issue.message}, received: ${issue.received}`,
      )
    } else {
      // biome-ignore lint/suspicious/noConsole: the config must be read before a logger can exist
      console.error(error)
    }
    process.exit(1)
  }
}
