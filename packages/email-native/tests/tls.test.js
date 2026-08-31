/*
 * Proves the trimmed mbedtls still does the one job it was kept for: a real
 * TLS 1.2/1.3 handshake against a relay, with the certificate verified.
 *
 * tls/gradido_mbedtls_config.h takes a lot out of mbedtls. A build that only
 * compiles proves nothing — this connects, verifies a chain and sends a mail.
 */
const test = require('node:test')
const assert = require('node:assert')
const tls = require('node:tls')
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const { execFileSync } = require('node:child_process')

const email = require('../index.cjs')

/** Self-signed, with an IP SAN so curl's hostname check passes for 127.0.0.1. */
function makeCert() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ge-tls-'))
  const key = path.join(dir, 'key.pem')
  const cert = path.join(dir, 'cert.pem')
  execFileSync(
    'openssl',
    [
      'req',
      '-x509',
      '-newkey',
      'rsa:2048',
      '-nodes',
      '-keyout',
      key,
      '-out',
      cert,
      '-days',
      '2',
      '-subj',
      '/CN=127.0.0.1',
      '-addext',
      'subjectAltName=IP:127.0.0.1',
    ],
    { stdio: 'ignore' },
  )
  return { dir, key, cert }
}

/** The same minimal sink as tests/fake-relay.js, but behind implicit TLS. */
function tlsRelay({ key, cert }) {
  let resolveMail
  const received = new Promise((r) => {
    resolveMail = r
  })
  const versions = []
  const server = tls.createServer(
    { key: fs.readFileSync(key), cert: fs.readFileSync(cert) },
    (socket) => {
      versions.push(socket.getProtocol())
      let inData = false
      let body = ''
      socket.write('220 fake ESMTP\r\n')
      socket.on('data', (chunk) => {
        if (inData) {
          body += chunk.toString('utf8')
          if (body.includes('\r\n.\r\n')) {
            inData = false
            socket.write('250 2.0.0 Ok\r\n')
            resolveMail(body)
          }
          return
        }
        for (const line of chunk.toString('utf8').split('\r\n').filter(Boolean)) {
          const verb = line.slice(0, 4).toUpperCase()
          if (verb === 'EHLO') {
            socket.write('250-fake\r\n250 8BITMIME\r\n')
          } else if (verb === 'DATA') {
            socket.write('354 go\r\n')
            inData = true
          } else if (verb === 'QUIT') {
            socket.write('221 Bye\r\n')
            socket.end()
          } else {
            socket.write('250 2.0.0 Ok\r\n')
          }
        }
      })
      socket.on('error', () => {
        /* a client that drops mid-session is the test's business, not the relay's */
      })
    },
  )
  return { server, received, versions }
}

const haveOpenssl = (() => {
  try {
    execFileSync('openssl', ['version'], { stdio: 'ignore' })
    return true
  } catch {
    return false
  }
})()

test('smtps: the trimmed mbedtls handshakes and verifies the chain', {
  skip: !haveOpenssl,
}, async (t) => {
  const ca = makeCert()
  t.after(() => fs.rmSync(ca.dir, { recursive: true, force: true }))

  const relay = tlsRelay(ca)
  await new Promise((r) => relay.server.listen(0, '127.0.0.1', r))
  const port = relay.server.address().port
  t.after(() => relay.server.close())

  const mailer = new email.Mailer({
    url: `smtps://127.0.0.1:${port}`, // implicit TLS, verified against our own CA
    from: 'noreply@gradido.net',
    fromName: 'Gradido',
    cainfo: ca.cert,
  })
  t.after(() => mailer.close())

  const sending = mailer.send('member@example.org', 'accountActivation', 'de', {
    firstName: 'Björn',
    lastName: 'Müller & Söhne',
    activationLink: 'https://gradido.net/activate?code=abc&t=1',
    hours: '23',
    minutes: '59',
    resendLink: 'https://gradido.net/resend?code=abc',
  })

  const body = await received(relay)
  assert.ok(body.includes('To: member@example.org'))
  await sending
  assert.equal(mailer.stats.sent, 1)
  assert.match(relay.versions[0], /^TLSv1\.[23]$/, `negotiated ${relay.versions[0]}`)
})

test('smtps: an unknown CA is refused, not ignored', { skip: !haveOpenssl }, async (t) => {
  const server = makeCert()
  const other = makeCert() // a CA the relay's certificate was not signed by
  let arrived = false
  t.after(() => {
    fs.rmSync(server.dir, { recursive: true, force: true })
    fs.rmSync(other.dir, { recursive: true, force: true })
  })

  const relay = tlsRelay(server)
  relay.received.then(() => {
    arrived = true
  })
  await new Promise((r) => relay.server.listen(0, '127.0.0.1', r))
  const port = relay.server.address().port
  t.after(() => relay.server.close())

  const mailer = new email.Mailer({
    url: `smtps://127.0.0.1:${port}`,
    from: 'noreply@gradido.net',
    cainfo: other.cert,
    timeoutMs: 1200,
  })
  t.after(() => mailer.close())

  /* The rejection is the assertion: the certificate is not the one the relay was signed by,
   * so mbedtls refuses the chain and curl says so. Nothing blocks this thread -- the send
   * runs on a pool thread and the relay needs this one to run its handshake. */
  await assert.rejects(
    mailer.sendMail({ to: 'member@example.org', subject: 's', body: 'b' }),
    /certificate|SSL|TLS/i,
  )

  /* What matters is that nothing was delivered. Whether the *server* saw a
   * connection is a property of the test harness, not of the client: Node's
   * tls server does not fire its callback before the client's alert arrives,
   * Bun's does. Both reject the certificate. */
  assert.equal(mailer.stats.sent, 0, 'nothing may be sent to an unverified relay')
  assert.equal(arrived, false, 'no message body may reach an unverified relay')
})

function received(relay) {
  return Promise.race([
    relay.received,
    new Promise((_, reject) => setTimeout(() => reject(new Error('no mail arrived')), 15_000)),
  ])
}
