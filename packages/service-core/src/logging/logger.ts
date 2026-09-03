import { mkdirSync } from 'node:fs'
import { dirname } from 'node:path'
import pino from 'pino'
import pretty from 'pino-pretty'
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

  return pino(options, pino.multistream(logStreams(env)))
}

/**
 * Where the lines go: stdout always, and a file when one is configured.
 *
 * **Streams and not pino transports.** A transport is a worker thread that pino starts by
 * *name* -- `target: 'pino-pretty'` -- and resolves at runtime, which needs a `node_modules`
 * to resolve it in. The single binary has none: `bun build --compile` puts every module
 * inside the executable, where nothing can be looked up by package name. A transport there
 * fails at the first line, so a bundled server would either start without its log file or
 * not start at all -- and shipping as one file is what the reference path is for. See
 * `scripts/bundle.ts`.
 *
 * The same streams are used when the server runs from a checkout, because two ways of
 * logging is one more than a thing needs, and the difference is not worth having: the same
 * bytes are written either way. What is given up is the worker thread that did the writing,
 * so a line is now formatted in the process that logged it. pino's own benchmark for that is
 * roughly a microsecond, and the destinations below are still buffered -- `sync: false`
 * means a write reaches the fd when the buffer fills or `flush()` asks for it, not while
 * the request waits.
 */
function logStreams(env: RuntimeConfig): pino.StreamEntry[] {
  const streams: pino.StreamEntry[] = []

  if (env.LOG_FILE) {
    mkdirSync(dirname(env.LOG_FILE), { recursive: true })
    streams.push({
      level: env.LOG_LEVEL,
      stream: pino.destination({ dest: env.LOG_FILE, mkdir: true, append: true, sync: false }),
    })
  }

  streams.push({
    level: env.LOG_LEVEL,
    stream:
      env.NODE_ENV === 'development'
        ? pretty({
            colorize: true,
            translateTime: 'SYS:standard',
            messageFormat: '[{cat}] {event} {msg}',
            sync: WATCHED,
          })
        : /* 1 is stdout. The JSON on stdout is the contracted format; whoever runs the
             process decides where it goes. */
          pino.destination({ dest: 1, sync: WATCHED }),
  })

  return streams
}

/**
 * Whether stdout is a terminal — whether somebody is reading the output as it appears.
 *
 * A buffered write reaches the fd when the buffer fills, which for a log going into a pipe or
 * a file is exactly right and costs nothing: nobody is comparing the moment a line arrives
 * with anything else. On a terminal somebody is, because the process also writes there
 * *directly* -- `setup/askForHomeCommunity.ts` prints a prompt and waits for an answer -- and
 * a direct write is not buffered. The two then arrive in the wrong order: the first start of a
 * server showed its question, and the migration line that came before it afterwards.
 *
 * So the terminal gets synchronous writes and everything else keeps the buffer. It is the case
 * where ordering is worth a syscall per line and where there is no throughput to protect: a
 * person is reading, at reading speed. A server logging into a pipe under load is the case
 * `sync: false` exists for, and it still has it.
 */
const WATCHED = process.stdout.isTTY === true
