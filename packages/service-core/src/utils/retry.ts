/**
 * Waiting for something that is not there yet.
 *
 * A service and the things it talks to start in whatever order the machine feels like:
 * the database is still replaying its write-ahead log while the backend is already up.
 * That is a timing problem, and a timing problem is solved by waiting, not by failing.
 *
 * What must not be retried is the other kind of failure -- a wrong password does not
 * become right by asking again. Which is which is the caller's knowledge, so it is a
 * parameter (`shouldRetry`) rather than something guessed here.
 */

export type RetryAttempt = {
  /** 1 for the first try. */
  attempt: number
  attempts: number
  error: unknown
  /** How long the retry waits before trying again. */
  delayMs: number
}

export type RetryOptions = {
  /** Total tries, including the first one. At least 1. */
  attempts: number
  /** Fixed wait between tries -- attempts * delayMs is the longest this can take. */
  delayMs: number
  /** Per try. Without it a connect to a host that drops packets hangs for minutes. */
  timeoutMs?: number
  /** False means the failure will not fix itself: give up now and throw it. */
  shouldRetry?: (error: unknown) => boolean
  /** Called before each wait, never after the last try. This is where logging goes. */
  onRetry?: (attempt: RetryAttempt) => void
  /** Names the operation in a timeout message. */
  label?: string
}

export class TimeoutError extends Error {
  public readonly timeoutMs: number

  public constructor(label: string, timeoutMs: number) {
    super(`${label} timed out after ${timeoutMs}ms`)
    this.name = 'TimeoutError'
    this.timeoutMs = timeoutMs
  }
}

export function wait(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms)
  })
}

/**
 * Runs an operation with a deadline.
 *
 * The operation is not cancelled when the deadline passes -- nothing here can cancel a
 * socket someone else owns. It is abandoned: its result is ignored and its eventual
 * failure is swallowed, because an unhandled rejection would end the process.
 */
export async function withTimeout<T>(
  operation: () => Promise<T>,
  timeoutMs: number,
  label = 'operation',
): Promise<T> {
  /* Wrapping the call turns a synchronous throw into a rejection, so both kinds of
     failure leave through the same door. */
  const pending = (async () => operation())()
  let timer: ReturnType<typeof setTimeout> | undefined

  try {
    return await Promise.race([
      pending,
      new Promise<never>((_resolve, reject) => {
        timer = setTimeout(() => reject(new TimeoutError(label, timeoutMs)), timeoutMs)
      }),
    ])
  } finally {
    clearTimeout(timer)
    pending.catch(() => {
      /* Abandoned: the deadline already decided this attempt's outcome. */
    })
  }
}

/**
 * Runs an operation until it succeeds, gives up, or hits something that will not fix
 * itself.
 *
 * The error that ends it is thrown as it was thrown: the caller reports what actually
 * went wrong, not a summary of how often it was asked.
 */
export async function retry<T>(operation: () => Promise<T>, options: RetryOptions): Promise<T> {
  const { attempts, delayMs, timeoutMs, shouldRetry, onRetry, label } = options
  if (attempts < 1) {
    throw new RangeError(`retry needs at least one attempt, got ${attempts}`)
  }

  let lastError: unknown
  for (let attempt = 1; attempt <= attempts; attempt++) {
    try {
      return timeoutMs === undefined
        ? await operation()
        : await withTimeout(operation, timeoutMs, label)
    } catch (error) {
      lastError = error
      if (shouldRetry !== undefined && !shouldRetry(error)) {
        throw error
      }
      if (attempt === attempts) {
        break
      }
      onRetry?.({ attempt, attempts, error, delayMs })
      await wait(delayMs)
    }
  }

  throw lastError
}
