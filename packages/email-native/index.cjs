'use strict'

/*
 * The addon, plus the two conveniences that are nicer to write in JS than in C:
 * a Mailer object around the external handle, and template metadata as a Map.
 *
 * Sending is one connection per mail and one promise per mail -- see napi/email_native.c
 * for why the addon does not use service-core's pooled mailer.
 */

const os = require('node:os')

const isBun = typeof process !== 'undefined' && 'bun' in process.versions
const isWindows = os.platform() === 'win32'

let native
try {
  // On Windows, Bun resolves the Node-API from bun.exe, so it gets its own
  // addon. Everywhere else one file serves both runtimes -- which is what
  // napi/exports.map buys, see the README.
  native =
    isBun && isWindows
      ? require('./build/email_native.bun.node')
      : require('./build/email_native.node')
} catch (cause) {
  throw new Error('the native addon is not built - run `turbo @gradido/email-native#build`', {
    cause,
  })
}

/*
 * How many sends may be on the thread pool at once.
 *
 * A send holds its pool thread for the whole SMTP session, and that pool is not ours: `fs.*`,
 * `dns.lookup`, zlib and `crypto.pbkdf2` take their threads from the same four. Filling all of
 * them with mail would stall every file read in the process for as long as the relay takes.
 *
 * So the default leaves one: UV_THREADPOOL_SIZE - 1, four being libuv's own default. Override
 * it with `maxConcurrent` when the process does nothing else -- or lower it when it does.
 *
 * Bun runs napi_create_async_work on a pool of its own and does not read UV_THREADPOOL_SIZE, so
 * there the number is a self-imposed ceiling rather than a reading of the pool. It is still the
 * right ceiling: what it limits is how many mails are in flight.
 */
function concurrencyLimit(config) {
  if (config && config.maxConcurrent > 0) {
    return Math.floor(config.maxConcurrent)
  }
  const pool = Number.parseInt(process.env.UV_THREADPOOL_SIZE ?? '', 10)
  const size = Number.isFinite(pool) && pool > 0 ? pool : 4
  return Math.max(1, size - 1)
}

/** Every template the binary carries, by name. */
const templates = new Map(native.templates().map((t) => [t.name, Object.freeze(t)]))

class Mailer {
  #handle
  #limit
  #inFlight = 0
  /** resolve() of every send that is waiting for a slot, oldest first. */
  #waiting = []

  constructor(config) {
    this.#handle = native.createMailer(config)
    this.#limit = concurrencyLimit(config)
  }

  /**
   * Runs @p start once a slot is free, in the order the sends were made.
   *
   * The gate sits here and not in C on purpose: a send that waits here holds a recipient and a
   * few values, while one queued on the C side would already hold its formatted message --
   * ~64 KB, images included. A thousand queued mails is the difference between a rounding error
   * and 64 MB.
   */
  async #withSlot(start) {
    if (this.#inFlight >= this.#limit) {
      await new Promise((release) => this.#waiting.push(release))
    }
    this.#inFlight++
    try {
      return await start()
    } finally {
      this.#inFlight--
      const next = this.#waiting.shift()
      if (next) {
        next()
      }
    }
  }

  /**
   * Renders the template and sends it as a multipart/related message -- the HTML plus the
   * six inline images it refers to. One connection, on a libuv thread pool thread.
   * The promise settles when the relay has taken the message (it resolves with the
   * Message-ID) or the attempt failed. There is no queue and no retry -- both are
   * the caller's, which is what having a promise per mail is for.
   */
  send(to, template, locale, values) {
    return this.#withSlot(() => native.sendTemplate(this.#handle, to, template, locale, values))
  }

  /** A message the caller rendered itself: `text`, `html`, or both. */
  sendMail(mail) {
    return this.#withSlot(() => native.sendMail(this.#handle, mail))
  }

  get stats() {
    /* `pending` is what the pool has, `waiting` is what this gate holds back. */
    return { ...native.stats(this.#handle), waiting: this.#waiting.length, limit: this.#limit }
  }

  /** Refuses further sends. Mails already out settle their promises first. */
  close() {
    native.closeMailer(this.#handle)
  }
}

module.exports = {
  render: native.render,
  renderBytes: native.renderBytes,
  templates,
  locales: native.locales(),
  assets: native.assets,
  limits: native.limits(),
  Mailer,
}
