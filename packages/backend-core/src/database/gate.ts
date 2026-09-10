/**
 * How long a request waits for a place at the database before it is told to come back later, and
 * what it is told: `contracts/database-config.json`, `rules.pool`, and `SERVICE_BUSY` in
 * `contracts/errors/api.json`. The fast path's numbers are the same -- `SC_DB_QUEUE_WAIT_MS`,
 * `SC_DB_QUEUE_PER_WORKER` and `SC_DB_RETRY_AFTER_S` in `service_core/db_exec.h`.
 */
export const DATABASE_WAIT_MS = 5000
export const DATABASE_QUEUE_PER_PLACE = 64
export const RETRY_AFTER_SECONDS = 1

/**
 * The database has all the work this process may give it, and this request's work was not
 * started. Thrown by {@link DatabaseGate.run}; the app answers it with 503 and `Retry-After`.
 *
 * Nothing of the work has run when this is thrown, which is what makes answering it with "try
 * again" honest: the same request a moment later cannot have been half done by this one.
 */
export class DatabaseBusy extends Error {
  public readonly retryAfter = RETRY_AFTER_SECONDS

  public constructor(reason: 'queue-full' | 'waited-too-long') {
    super(
      reason === 'queue-full'
        ? 'every place at the database is taken and the queue in front of them is full'
        : `no place at the database came free within ${DATABASE_WAIT_MS} ms`,
    )
    this.name = 'DatabaseBusy'
  }
}

type Waiter = {
  readonly resolve: () => void
  readonly reject: (error: DatabaseBusy) => void
  timer: ReturnType<typeof setTimeout> | undefined
}

/**
 * A fixed number of places in front of the database, and a bounded, timed queue for the rest.
 *
 * bun's SQL client has a pool of its own and queues a query when every connection is busy -- but
 * without end, and there is no option that bounds it: `idleTimeout`, despite its description,
 * closes a connection on which nothing arrives for that long, running statements included. So
 * the bound lives here, one step before bun's pool. There are exactly as many places as bun has
 * connections, so a request holding a place always finds a connection free and bun never queues
 * anything; whoever does not get a place waits here, where the wait can be refused.
 *
 * An Interaction takes one place for all of its statements -- a registration's retries included
 * -- and gives it back when it is done, by returning or by throwing.
 */
export class DatabaseGate {
  private free: number
  private readonly waiting: Waiter[] = []

  public constructor(
    private readonly places: number,
    private readonly waitMs: number = DATABASE_WAIT_MS,
    private readonly queueLimit: number = places * DATABASE_QUEUE_PER_PLACE,
  ) {
    this.free = places
  }

  /** Runs @p work holding a place. Throws {@link DatabaseBusy} without running it when no place
   *  comes free in time or the queue is already full. */
  public async run<T>(work: () => Promise<T>): Promise<T> {
    await this.take()
    try {
      return await work()
    } finally {
      this.give()
    }
  }

  /** How many requests are waiting for a place, for a test or a log line. */
  public get queued(): number {
    return this.waiting.length
  }

  private take(): Promise<void> {
    if (this.free > 0) {
      this.free -= 1
      return Promise.resolve()
    }
    if (this.waiting.length >= this.queueLimit) {
      return Promise.reject(new DatabaseBusy('queue-full'))
    }
    return new Promise<void>((resolve, reject) => {
      const waiter: Waiter = { resolve, reject, timer: undefined }
      waiter.timer = setTimeout(() => {
        const at = this.waiting.indexOf(waiter)
        if (at !== -1) {
          this.waiting.splice(at, 1)
          reject(new DatabaseBusy('waited-too-long'))
        }
      }, this.waitMs)
      this.waiting.push(waiter)
    })
  }

  /* A place given back goes straight to the longest waiter, if there is one -- never through
     `free`, where a request that arrived later could take it first. */
  private give(): void {
    const next = this.waiting.shift()
    if (next === undefined) {
      this.free = Math.min(this.free + 1, this.places)
      return
    }
    clearTimeout(next.timer)
    next.resolve()
  }
}
