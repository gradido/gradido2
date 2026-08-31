/*
 * What the addon does inside a host process. Three things, and nothing else:
 *
 *   1. the renderer produces what pug produces -- every template, locale and branch
 *      variant, all three parts, compared against the snapshots. That is the whole
 *      matrix, the same 810 documents `zig build check` holds the C binary to;
 *   2. sc_mailer runs inside the Node process -- its own threads, its own libcurl --
 *      and a mail actually arrives;
 *   3. loading it does not disturb Node's own TLS.
 *
 * The handshake against a real relay is next door in tls.test.js.
 */
const test = require('node:test')
const assert = require('node:assert')
const net = require('node:net')
const fs = require('node:fs')
const path = require('node:path')

const email = require('../index.cjs')

test('introspection matches what the templates declare', () => {
  assert.equal(email.templates.size, 17)
  assert.equal(email.locales.length, 10)
  const a = email.templates.get('accountActivation')
  assert.deepEqual(a.slots.slice().sort(), [
    'activationLink',
    'firstName',
    'hours',
    'lastName',
    'logoUrl',
    'minutes',
    'resendLink',
  ])
  assert.equal(a.variants, 4)
  assert.deepEqual(email.templates.get('emailChangeSupport').flags, ['typoCorrection', 'takeBack'])
})

test('the buffer limits are the build-time constants', () => {
  assert.equal(email.limits.maxStaticHtml, 21962)
  assert.equal(email.limits.maxStaticText, 2011)
  assert.equal(email.limits.maxSlotRefs, 13)
})

test('the inline images are in the binary', () => {
  const assets = email.assets()
  assert.equal(assets.length, 6)
  const header = assets.find((a) => a.cid === 'gradidoheader')
  assert.equal(header.size, header.data.length)
  assert.equal(header.data.subarray(1, 4).toString('latin1'), 'PNG')
})

/* The snapshots are the checked-in pug output; tests/snapshots.test.mjs is what
 * keeps them equal to what pug renders today. Comparing the addon against them is
 * therefore a comparison against pug, without this file having to run pug. */
const snapshots = path.join(__dirname, '__snapshots__')

test('render() equals the snapshots, for every template, locale and variant', async () => {
  /* render() has no variant argument -- a variant is selected by which values are
   * set, exactly as the templates' `if`s do it. tools/variants.mjs turns the branch
   * table into those values, so the numbering here is the one the C uses and the one
   * the snapshot filenames carry. Getting that wrong does not pass quietly: the bytes
   * would be another variant's. */
  const { variantValues } = await import('../tools/variants.mjs')
  let checked = 0

  for (const [name, info] of email.templates) {
    const perVariant = variantValues(name, info.slots)
    assert.equal(
      perVariant.length,
      info.variants,
      `${name}: ${perVariant.length} variants in the branch table, ${info.variants} in the binary`,
    )

    for (const locale of email.locales) {
      perVariant.forEach((values, combo) => {
        const got = email.render(name, locale, values)
        const at = `${name}/${locale}.${combo}`
        for (const kind of ['html', 'subject', 'text']) {
          const want = fs.readFileSync(
            path.join(snapshots, name, `${locale}.${combo}.${kind}`),
            'utf8',
          )
          assert.ok(got[kind] === want, `${at}: ${kind} differs from the snapshot`)
          checked++
        }
      })
    }
  }
  // The whole matrix: 17 templates x 10 locales x variants x {html, subject, text}.
  assert.equal(checked, 810)
})

test('an unset value drops its if-branch', () => {
  const values = {
    firstName: 'A',
    lastName: 'B',
    activationLink: 'x',
    hours: '23',
    minutes: '59',
    resendLink: 'y',
  }
  const without = email.render('accountActivation', 'de', values)
  const with_ = email.render('accountActivation', 'de', {
    ...values,
    logoUrl: 'https://x/banner.png',
  })
  assert.ok(!without.html.includes('alt="Banner"'))
  assert.ok(with_.html.includes('alt="Banner"'))
})

test('a recipient carrying a newline is refused, not delivered', async () => {
  /* Header injection: a bare LF in the recipient used to put a Bcc: of the caller's choosing
   * into the message, and a relay delivered it. service-core's email/message.c refuses every
   * control character in an address now; this asserts the refusal reaches JavaScript as a
   * rejection rather than as a throw -- `send(...).catch()` has to see it. */
  const mailer = new email.Mailer({ url: 'smtp://127.0.0.1:1', from: 'noreply@gradido.net' })
  try {
    await assert.rejects(
      mailer.sendMail({
        to: 'victim@example.org\nBcc: attacker@evil.test',
        subject: 's',
        body: 'b',
      }),
      /control character/,
    )
  } finally {
    mailer.close()
  }
})

/* ------------------------------------------------------------------ SMTP */

/** The smallest relay that will take one message. Resolves with what it received. */
function fakeRelay() {
  let resolveMail
  const received = new Promise((r) => {
    resolveMail = r
  })
  const server = net.createServer((socket) => {
    let inData = false
    let body = ''
    socket.write('220 fake ESMTP\r\n')
    socket.on('data', (chunk) => {
      if (inData) {
        body += chunk.toString('utf8')
        if (body.includes('\r\n.\r\n')) {
          inData = false
          socket.write('250 2.0.0 Ok: queued\r\n')
          resolveMail(body)
        }
        return
      }
      for (const line of chunk.toString('utf8').split('\r\n').filter(Boolean)) {
        const verb = line.slice(0, 4).toUpperCase()
        if (verb === 'EHLO') {
          socket.write('250-fake\r\n250 8BITMIME\r\n')
        } else if (verb === 'HELO' || verb === 'MAIL' || verb === 'RCPT') {
          socket.write('250 Ok\r\n')
        } else if (verb === 'DATA') {
          socket.write('354 End data with <CR><LF>.<CR><LF>\r\n')
          inData = true
        } else if (verb === 'QUIT') {
          socket.write('221 Bye\r\n')
          socket.end()
        } else {
          socket.write('250 Ok\r\n')
        }
      }
    })
    socket.on('error', () => {
      /* a client that drops mid-session is the test's business, not the relay's */
    })
  })
  return { server, received }
}

/* Runs on both runtimes, and the promise is the whole point: it settles when the relay has
 * taken the message. The addon links no libuv of its own any more -- the send runs on a
 * thread of the host's own pool through napi_create_async_work -- which is what took the
 * uv_* symbol collision with Bun's shim (oven-sh/bun#18546) off the table for good. */
/*
 * The pool is not ours: fs, dns, zlib and crypto take their threads from the same four, and a
 * send holds its thread for the whole SMTP session. So the wrapper keeps one free -- and what
 * proves it is the relay counting connections, not the counter the wrapper keeps itself.
 */
test('never uses more than its share of the thread pool', async (t) => {
  let open = 0
  let peak = 0
  const server = net.createServer((socket) => {
    open++
    peak = Math.max(peak, open)
    socket.on('close', () => open--)
    let inData = false
    let body = ''
    socket.write('220 fake ESMTP\r\n')
    socket.on('data', (chunk) => {
      const text = chunk.toString('utf8')
      if (inData) {
        body += text
        /* Slow on purpose: without a pause every send finishes before the next begins and the
         * peak would be 1 whatever the limit is. */
        if (body.includes('\r\n.\r\n')) {
          inData = false
          setTimeout(() => socket.write('250 Ok\r\n'), 40)
        }
        return
      }
      for (const line of text.split('\r\n').filter(Boolean)) {
        const verb = line.slice(0, 4).toUpperCase()
        if (verb === 'EHLO') socket.write('250-fake\r\n250 8BITMIME\r\n')
        else if (verb === 'DATA') {
          socket.write('354 go\r\n')
          inData = true
        } else if (verb === 'QUIT') {
          socket.write('221 Bye\r\n')
          socket.end()
        } else socket.write('250 Ok\r\n')
      }
    })
    socket.on('error', () => {
      /* a client that drops mid-session is the test's business, not the relay's */
    })
  })
  await new Promise((r) => server.listen(0, '127.0.0.1', r))
  t.after(() => server.close())

  const mailer = new email.Mailer({
    url: `smtp://127.0.0.1:${server.address().port}`,
    from: 'noreply@gradido.net',
    starttls: 0,
    maxConcurrent: 2,
  })
  t.after(() => mailer.close())
  assert.equal(mailer.stats.limit, 2)

  /* Sampled rather than snapshotted at one instant: a single reading after a fixed delay says
   * as much about how busy the machine was as about the gate. */
  let maxPending = 0
  const sampler = setInterval(() => {
    maxPending = Math.max(maxPending, mailer.stats.pending)
  }, 3)

  const jobs = []
  for (let i = 0; i < 10; i++)
    jobs.push(mailer.sendMail({ to: `m${i}@example.org`, subject: 's', text: 'b' }))

  await Promise.all(jobs)
  clearInterval(sampler)

  assert.equal(mailer.stats.sent, 10)
  assert.equal(mailer.stats.waiting, 0, 'nothing left holding back')
  assert.ok(maxPending <= 2, `${maxPending} sends were on the pool at once, the limit was 2`)
  /* The relay's own count, and it may legitimately be one higher: a socket the client has
   * closed is not gone until this process has run the 'close' handler, so the next send's
   * connection can arrive first. What must not happen is the gate letting a third *send* run,
   * which is what maxPending above measures exactly. */
  assert.ok(peak <= 3, `the relay saw ${peak} connections at once, the limit was 2`)
})

test('a send settles its promise when the relay has the mail', async (t) => {
  const { server, received } = fakeRelay()
  await new Promise((r) => server.listen(0, '127.0.0.1', r))
  const port = server.address().port
  t.after(() => server.close())

  const mailer = new email.Mailer({
    url: `smtp://127.0.0.1:${port}`,
    from: 'noreply@gradido.net',
    fromName: 'Gradido',
    starttls: 0, // the fake relay offers none
  })
  t.after(() => mailer.close())

  const msgid = await mailer.send('member@example.org', 'accountActivation', 'de', {
    firstName: 'Björn',
    lastName: 'Müller & Söhne',
    activationLink: 'https://gradido.net/activate?code=abc&t=1',
    hours: '23',
    minutes: '59',
    resendLink: 'https://gradido.net/resend?code=abc',
  })

  const body = await received
  assert.ok(body.includes('To: member@example.org'), 'recipient header')
  assert.ok(body.includes(`Message-ID: <${msgid}>`), 'the promise resolved with the Message-ID')
  assert.ok(body.includes('Gradido'), 'rendered body arrived')

  const s = mailer.stats
  assert.equal(s.sent, 1)
  assert.equal(s.failed, 0)
  assert.equal(s.pending, 0, 'nothing left in flight once the promise settled')
})

/*
 * What actually leaves the process, checked against the RFCs rather than against the code that
 * wrote it. Every line here was a defect once: the document went out as text/plain, the six
 * images the templates name were never attached, the UTF-8 was unlabelled 8-bit, and the lines
 * were up to 3294 bytes where RFC 5322 2.1.1 allows 998.
 */
test('the message on the wire is a conformant MIME document', async (t) => {
  const { server, received } = fakeRelay()
  await new Promise((r) => server.listen(0, '127.0.0.1', r))
  t.after(() => server.close())

  const mailer = new email.Mailer({
    url: `smtp://127.0.0.1:${server.address().port}`,
    from: 'noreply@gradido.net',
    fromName: 'Gradido',
    starttls: 0,
  })
  t.after(() => mailer.close())

  const values = {
    firstName: 'Björn',
    lastName: 'Müller & Söhne',
    activationLink: 'https://gradido.net/activate?code=abc',
    resendLink: 'https://gradido.net/resend?code=abc',
    hours: '23',
    minutes: '59',
  }
  await mailer.send('member@example.org', 'accountActivation', 'de', values)
  /* SMTP transparency undone: curl doubles a dot at the start of a line, RFC 5321 4.5.2. */
  const wire = (await received).replace(/\r\n\.\./g, '\r\n.')

  assert.match(wire, /Content-Type: multipart\/alternative; boundary="/)
  assert.match(wire, /Content-Type: text\/plain; charset=utf-8/)
  assert.match(wire, /Content-Type: multipart\/related; type="text\/html"; boundary="/)
  assert.match(wire, /Content-Type: text\/html; charset=utf-8/)
  assert.match(wire, /Content-Transfer-Encoding: quoted-printable/)
  /* RFC 2046 5.1.4: the richer alternative comes last. */
  assert.ok(wire.indexOf('text/plain') < wire.indexOf('text/html'), 'text before html')

  /* The six inline images, each with the Content-ID its cid: reference names. */
  const ids = [...wire.matchAll(/^Content-ID: <(.+)>$/gm)].map((m) => m[1])
  assert.deepEqual(
    ids.sort(),
    email
      .assets()
      .map((a) => a.cid)
      .sort(),
  )

  /* 7-bit and inside the line limit -- what the transfer encoding is for. */
  const lines = wire.split('\r\n')
  assert.ok(Math.max(...lines.map((l) => l.length)) <= 998, 'RFC 5322 2.1.1: 998 bytes a line')
  assert.equal([...Buffer.from(wire, 'utf8')].filter((b) => b > 126).length, 0, '7-bit clean')

  /* And it decodes back to exactly what the renderer produced -- an encoding that loses a byte
   * would pass every assertion above. */
  const boundary = /boundary="([^"]+_rel)"/.exec(wire)[1]
  const html = wire.split(`--${boundary}`).find((p) => /Content-Type: text\/html/.test(p))
  const decoded = Buffer.from(
    html
      .slice(html.indexOf('\r\n\r\n') + 4)
      .replace(/=\r\n/g, '')
      .replace(/=([0-9A-F]{2})/g, (_, h) => String.fromCharCode(Number.parseInt(h, 16))),
    'latin1',
  ).toString('utf8')
  const norm = (x) => x.replace(/\r\n/g, '\n').replace(/\n+$/, '')
  assert.equal(norm(decoded), norm(email.render('accountActivation', 'de', values).html))
})

test("the addon's TLS does not disturb Node's own", async () => {
  /* mbedtls is linked in statically. If it had collided with Node's OpenSSL this
   * would be where it showed -- crypto still has to work after the addon loaded. */
  const crypto = require('node:crypto')
  assert.equal(crypto.createHash('sha256').update('gradido').digest('hex').length, 64)
  const tls = require('node:tls')
  assert.ok(tls.getCiphers().length > 0)
})
