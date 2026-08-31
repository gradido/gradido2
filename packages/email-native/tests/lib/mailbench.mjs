/*
 * Shared by tests/mail-bench.mjs (the full table) and tests/bench.test.mjs (a
 * short run inside the test suite). Sending only: the message is rendered once
 * up front, because rendering is measured in tests/render-bench.mjs.
 */
import { spawn, execFileSync } from 'node:child_process'
import { createRequire } from 'node:module'
import net from 'node:net'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import nodemailer from 'nodemailer'

const require = createRequire(import.meta.url)
export const email = require('../../index.cjs')

const RELAY = fileURLToPath(new URL('../fake-relay.js', import.meta.url))

export const haveOpenssl = (() => {
  try {
    execFileSync('openssl', ['version'], { stdio: 'ignore' })
    return true
  } catch {
    return false
  }
})()

/** Self-signed, with an IP SAN so both clients accept it for 127.0.0.1. */
export function makeCert() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ge-bench-'))
  const key = path.join(dir, 'key.pem')
  const cert = path.join(dir, 'cert.pem')
  execFileSync('openssl', [
    'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
    '-keyout', key, '-out', cert, '-days', '2', '-subj', '/CN=127.0.0.1',
    '-addext', 'subjectAltName=IP:127.0.0.1',
  ], { stdio: 'ignore' })
  return { dir, key, cert, cleanup: () => fs.rmSync(dir, { recursive: true, force: true }) }
}

/** The sink, in its own process, so neither client competes with it for a loop. */
export async function startRelay(ca) {
  const args = ca ? [RELAY, '--key', ca.key, '--cert', ca.cert] : [RELAY]
  const child = spawn(process.execPath, args, { stdio: ['ignore', 'pipe', 'pipe'] })
  let counted = null
  child.stderr.on('data', (b) => {
    const m = /RELAY (\d+)/.exec(b.toString())
    if (m) counted = Number(m[1])
  })
  const port = await new Promise((resolve) => {
    child.stdout.on('data', (b) => {
      const m = /PORT (\d+)/.exec(b.toString())
      if (m) resolve(Number(m[1]))
    })
  })
  return {
    port,
    async stop() {
      child.kill('SIGTERM')
      await new Promise((r) => child.on('exit', r))
      return counted
    },
  }
}

/* curl sets TCP_NODELAY on every connection; Node does not, and nodemailer does
 * not ask it to. Against a relay that answers immediately that costs a
 * delayed-ACK stall per message, so nodemailer is measured both ways rather
 * than pretending the default is the whole story. Bun already sets it, which is
 * why its two rows agree. */
let origConnect = null
export function forceNoDelay(on) {
  if (on && !origConnect) {
    origConnect = net.Socket.prototype.connect
    net.Socket.prototype.connect = function (...a) {
      const r = origConnect.apply(this, a)
      this.setNoDelay(true)
      return r
    }
  } else if (!on && origConnect) {
    net.Socket.prototype.connect = origConnect
    origConnect = null
  }
}

export function renderOne() {
  return email.render('accountActivation', 'de', {
    firstName: 'Björn',
    lastName: 'Müller & Söhne',
    activationLink: 'https://gradido.net/activate?code=abcdef0123456789abcdef&t=1',
    hours: '23',
    minutes: '59',
    resendLink: 'https://gradido.net/resend?code=abcdef0123456789abcdef',
  })
}

// ---------------------------------------------------------------- native
export function benchNative({ port, n, connections, ca, mail }) {
  const mailer = new email.Mailer({
    url: `${ca ? 'smtps' : 'smtp'}://127.0.0.1:${port}`,
    from: 'noreply@gradido.net',
    fromName: 'Gradido',
    starttls: 0, // the relay offers none; smtps is TLS from the first byte
    cainfo: ca ? ca.cert : undefined,
    workers: connections,
    workerMax: connections,
    queueMax: n,
  })
  const t0 = process.hrtime.bigint()
  for (let i = 0; i < n; i++) {
    mailer.enqueue({ to: `m${i}@example.org`, subject: mail.subject, body: mail.html })
  }
  const drained = mailer.drain(180_000)
  const ns = Number(process.hrtime.bigint() - t0)
  const stats = mailer.stats
  mailer.close()
  return { ns, sent: stats.sent, failed: stats.failed, drained }
}

// ------------------------------------------------------------ nodemailer
export async function benchNodemailer({ port, n, connections, ca, mail }) {
  const transport = nodemailer.createTransport({
    host: '127.0.0.1',
    port,
    secure: !!ca,
    ignoreTLS: !ca,
    tls: ca ? { ca: fs.readFileSync(ca.cert) } : undefined,
    pool: true,
    maxConnections: connections,
    maxMessages: Infinity,
  })
  let sent = 0
  let failed = 0
  const t0 = process.hrtime.bigint()
  const jobs = []
  for (let i = 0; i < n; i++) {
    jobs.push(
      transport
        .sendMail({
          from: '"Gradido" <noreply@gradido.net>',
          to: `m${i}@example.org`,
          subject: mail.subject,
          html: mail.html,
        })
        .then(() => sent++)
        .catch(() => failed++),
    )
  }
  await Promise.all(jobs)
  const ns = Number(process.hrtime.bigint() - t0)
  transport.close()
  return { ns, sent, failed, drained: true }
}

/**
 * One column of the table: native, nodemailer as configured, and nodemailer with
 * TCP_NODELAY, each against a fresh relay.
 */
export async function compare({ n, connections, ca, mail }) {
  const run = async (fn) => {
    const relay = await startRelay(ca)
    const r = await fn({ port: relay.port, n, connections, ca, mail })
    r.relay = await relay.stop()
    r.perMail = r.ns / n
    r.rate = 1e9 / r.perMail
    return r
  }

  const native = await run(benchNative)
  const nodemailerDefault = await run(benchNodemailer)
  forceNoDelay(true)
  const nodemailerNoDelay = await run(benchNodemailer)
  forceNoDelay(false)

  return { native, nodemailerDefault, nodemailerNoDelay }
}

export function formatRow(label, r) {
  const ok = r.sent === r.relay && r.failed === 0 ? '' : `  !! sent ${r.sent}, relay ${r.relay}, failed ${r.failed}`
  return (
    `  ${label.padEnd(26)} ${(r.perMail / 1000).toFixed(0).padStart(6)} us/mail  ` +
    `${r.rate.toFixed(0).padStart(7)} mails/s${ok}`
  )
}
