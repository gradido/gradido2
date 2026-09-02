import { mkdirSync } from 'node:fs'
import { dirname } from 'node:path'
import pino from 'pino'
import type { RuntimeConfig } from '..'

/**
 * The closed category set of contracts/logging.json. A category names a place in the
 * system, not a place in the source tree -- legacy's per-class log4js categories are not
 * carried over. Adding one here is a contract change.
 */
export type LogCategory =
  | 'auth'
  | 'user'
  | 'transaction'
  | 'contribution'
  | 'community'
  | 'federation'
  | 'http'
  | 'db'
  | 'session'
  | 'startup'
  | 'mail'

/** { code, name } from contracts/errors, never the thrown object and never a stack. */
export type LogError = {
  code: number
  name: string
}

/**
 * One log line, minus the envelope pino fills in (time, level, msg).
 *
 * A log line is a structured event with a human sentence attached, not a sentence with
 * fields attached: `event` is what the contract tests assert on, `msg` is never compared.
 */
export type LogLine = {
  cat: LogCategory
  event: string
  err?: LogError
  /* Event-specific fields, flat, one level of nesting at most. One event id always
     carries the same keys. */
  data?: Record<string, unknown>
}

/** Fields that stay attached for as long as the child logger lives. */
export type LogBindings = {
  /* Request correlation id, on every line emitted while serving a request. */
  req?: string
  /* users.id -- never the email, never the gradido_id. */
  usr?: number | bigint
}

/**
 * The only way to write a log line in a Gradido service.
 *
 * It is a facade rather than the bare pino logger because the envelope is contracted:
 * no top-level field beyond the ones in contracts/logging.json, and everything
 * event-specific inside `data`. A typed surface makes that rule hold at compile time
 * instead of at review time.
 */
export class Logger {
  private readonly pino: pino.Logger

  private constructor(logger: pino.Logger) {
    this.pino = logger
  }

  public static create(env: RuntimeConfig): Logger {
    return new Logger(createPinoLogger(env))
  }

  /** A logger that carries req/usr on every line, for the lifetime of one request. */
  public child(bindings: LogBindings): Logger {
    /* JSON has no 64 bit integer, so usr crosses as a number. users.id stays far below
       2^53 in every installation that exists; the alternative is a string, and that would
       change the contracted type. */
    return new Logger(
      this.pino.child({
        ...(bindings.req === undefined ? {} : { req: bindings.req }),
        ...(bindings.usr === undefined ? {} : { usr: Number(bindings.usr) }),
      }),
    )
  }

  public trace(line: LogLine, msg: string): void {
    this.pino.trace(line, msg)
  }

  public debug(line: LogLine, msg: string): void {
    this.pino.debug(line, msg)
  }

  public info(line: LogLine, msg: string): void {
    this.pino.info(line, msg)
  }

  public warn(line: LogLine, msg: string): void {
    this.pino.warn(line, msg)
  }

  public error(line: LogLine, msg: string): void {
    this.pino.error(line, msg)
  }

  public fatal(line: LogLine, msg: string): void {
    this.pino.fatal(line, msg)
  }

  /** Writes out what is still buffered. Called on shutdown, not per line. */
  public flush(): void {
    this.pino.flush()
  }
}

function createPinoLogger(env: RuntimeConfig): pino.Logger {
  /* base: null removes pid and hostname. They differ per process, so two identical runs
     would compare unequal -- see contracts/logging.json, envelope rules. pino's default
     timestamp is already unix milliseconds, which is what the contract asks for. */
  const options: pino.LoggerOptions = { level: env.LOG_LEVEL, base: null }

  try {
    return pino({ ...options, transport: { targets: transportTargets(env) } })
  } catch (error) {
    /* Transports resolve their targets dynamically, which a bundled build can defeat.
       Losing pretty printing or the log file is survivable; starting without a logger is
       not. */
    // biome-ignore lint/suspicious/noConsole: this is the failure of the logger itself
    console.warn('log transport unavailable, falling back to plain stdout:', error)
    return pino(options)
  }
}

function transportTargets(env: RuntimeConfig): pino.TransportTargetOptions[] {
  const targets: pino.TransportTargetOptions[] = []

  if (env.LOG_FILE) {
    mkdirSync(dirname(env.LOG_FILE), { recursive: true })
    targets.push({
      target: 'pino/file',
      level: env.LOG_LEVEL,
      options: { destination: env.LOG_FILE, mkdir: true, append: true, sync: false },
    })
  }

  targets.push(
    env.NODE_ENV === 'development'
      ? {
          target: 'pino-pretty',
          level: env.LOG_LEVEL,
          options: {
            colorize: true,
            translateTime: 'SYS:standard',
            messageFormat: '[{cat}] {event} {msg}',
          },
        }
      : {
          target: 'pino/file',
          level: env.LOG_LEVEL,
          /* 1 is stdout. The JSON on stdout is the contracted format; whoever runs the
             process decides where it goes. */
          options: { destination: 1, sync: false },
        },
  )

  return targets
}
