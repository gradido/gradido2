import type { LogLine } from '../logging'

/** How deep a wrapped error is followed before giving up; a cause chain is data from a library. */
const CAUSE_DEPTH_MAX = 4

/**
 * The process cannot continue, and here is everything that led to that.
 *
 * Thrown where the damage is found and decided on in one place per service -- the HTTP error
 * handler -- which hands it to `stopAfterCriticalError`. It carries the contracted fatal event
 * it is reported as, because whoever throws it knows what broke and whoever catches it does not.
 *
 * In C++ terms: the exception type that reaches `main` and is answered by an orderly exit,
 * as opposed to one a request handler turns into a 500 and forgets.
 */
export class CriticalError extends AggregateError {
  public constructor(
    /** `contracts/logging.json`, level fatal. Written once per error held. */
    public readonly line: LogLine,
    message: string,
    errors: readonly unknown[],
  ) {
    super(errors, message)
    this.name = 'CriticalError'
  }

  /**
   * What each error said, one line each. The deepest cause speaks: a wrapper above it repeats
   * the statement and its parameters, which a log line must not carry.
   */
  public reasons(): string[] {
    return this.errors.map((error: unknown) => {
      let at: unknown = error
      let message = String(error)
      for (let depth = 0; depth < CAUSE_DEPTH_MAX && at !== null && at !== undefined; depth++) {
        const said = (at as { message?: unknown }).message
        if (typeof said === 'string' && said.length > 0) {
          message = said
        }
        at = (at as { cause?: unknown }).cause
      }
      return message.replace(/\s+/gu, ' ').trim()
    })
  }
}
