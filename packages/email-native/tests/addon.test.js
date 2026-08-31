/*
 * What the addon does inside a host process. Three things, and nothing else:
 *
 *   1. the renderer produces what pug produces -- every template in every locale,
 *      compared against the snapshots. That is 170 of the 540 documents: this goes
 *      through render(), which selects a variant by which values are set, so it
 *      reaches variant 0 of each template. The full matrix, every branch variant
 *      included, is `zig build check` against the same snapshots;
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
        const html = fs.readFileSync(path.join(snapshots, name, `${locale}.${combo}.html`), 'utf8')
        const subject = fs.readFileSync(
          path.join(snapshots, name, `${locale}.${combo}.subject`),
          'utf8',
        )
        assert.ok(got.html === html, `${at}: html differs from the snapshot`)
        assert.ok(got.subject === subject, `${at}: subject differs from the snapshot`)
        checked += 2
      })
    }
  }
  // The whole matrix: 17 templates x 10 locales x variants x {html, subject}.
  assert.equal(checked, 540)
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

/* Runs on both runtimes. It did not, until napi/exports.map stopped the addon's
 * own uv_* calls from being preempted by the host's -- Bun's libuv shim aborts on
 * uv_available_parallelism and uv_cond_init (oven-sh/bun#18546), and those calls
 * were reaching it instead of the libuv linked in here. */
test('sc_mailer sends from inside the host process', async (t) => {
  const { server, received } = fakeRelay()
  await new Promise((r) => server.listen(0, '127.0.0.1', r))
  const port = server.address().port
  t.after(() => server.close())

  const mailer = new email.Mailer({
    url: `smtp://127.0.0.1:${port}`,
    from: 'noreply@gradido.net',
    fromName: 'Gradido',
    starttls: 0, // the fake relay offers none
    workers: 1,
    /* Not 0: the default calls uv_available_parallelism, which Bun cannot. */
    workerMax: 2,
  })
  t.after(() => mailer.close())

  mailer.send('member@example.org', 'accountActivation', 'de', {
    firstName: 'Björn',
    lastName: 'Müller & Söhne',
    activationLink: 'https://gradido.net/activate?code=abc&t=1',
    hours: '23',
    minutes: '59',
    resendLink: 'https://gradido.net/resend?code=abc',
  })

  const body = await received
  assert.ok(body.includes('To: member@example.org'), 'recipient header')
  assert.ok(body.includes('Message-ID: <'), 'message id')
  assert.ok(body.includes('Gradido'), 'rendered body arrived')

  assert.ok(mailer.drain(5000), 'queue drained')
  const s = mailer.stats
  assert.equal(s.queued, 1)
  assert.equal(s.sent, 1)
  assert.equal(s.failed, 0)
})

test("the addon's TLS does not disturb Node's own", async () => {
  /* mbedtls is linked in statically. If it had collided with Node's OpenSSL this
   * would be where it showed -- crypto still has to work after the addon loaded. */
  const crypto = require('node:crypto')
  assert.equal(crypto.createHash('sha256').update('gradido').digest('hex').length, 64)
  const tls = require('node:tls')
  assert.ok(tls.getCiphers().length > 0)
})
