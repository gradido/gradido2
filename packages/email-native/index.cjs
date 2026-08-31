'use strict'

/*
 * The addon, plus the two conveniences that are nicer to write in JS than in C:
 * a Mailer object around the external handle, and template metadata as a Map.
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

/** Every template the binary carries, by name. */
const templates = new Map(native.templates().map((t) => [t.name, Object.freeze(t)]))

class Mailer {
  #handle

  constructor(config) {
    this.#handle = native.createMailer(config)
  }

  /** Renders the template and queues it. Does not dial and does not block. */
  send(to, template, locale, values) {
    native.sendTemplate(this.#handle, to, template, locale, values)
  }

  /** A message the caller rendered itself. `body` is sent as text/plain. */
  enqueue(mail) {
    native.enqueue(this.#handle, mail)
  }

  get stats() {
    return native.stats(this.#handle)
  }

  /** Blocks until the queue is empty or the timeout passes. Shutdown and tests. */
  drain(timeoutMs = 5000) {
    return native.drain(this.#handle, { timeoutMs })
  }

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
