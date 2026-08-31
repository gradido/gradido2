/*
 * The send benchmark as a test: a short run over TLS, so `npm test` says both
 * clients still deliver everything and roughly how they compare. The numbers to
 * quote come from tests/mail-bench.mjs, which runs longer and does plain SMTP
 * as well.
 *
 * The assertions are deliberately weak. A benchmark that fails the suite
 * because a machine was busy is worse than no benchmark; what must hold is that
 * every mail arrives, over a verified TLS connection, from both clients.
 *
 *   N=2000 node --test tests/bench.test.mjs
 */
import test from 'node:test'
import assert from 'node:assert'
import { compare, formatRow, haveOpenssl, makeCert, renderOne } from './lib/mailbench.mjs'

const N = Number(process.env.N ?? 200)
const CONNECTIONS = Number(process.env.CONNECTIONS ?? 4)

test('smtps: sc_mailer and nodemailer both deliver, and sc_mailer is faster', {
  skip: !haveOpenssl && 'no openssl to make a certificate with',
  timeout: 180_000,
}, async () => {
  const mail = renderOne()
  const ca = makeCert()
  try {
    const r = await compare({ n: N, connections: CONNECTIONS, ca, mail })

    console.log(`\n  ${N} mails over smtps, ${mail.html.length} B body, ${CONNECTIONS} connections`)
    console.log(formatRow('sc_mailer (addon)', r.native))
    console.log(formatRow('nodemailer', r.nodemailerDefault))
    console.log(formatRow('nodemailer + TCP_NODELAY', r.nodemailerNoDelay))

    for (const [name, x] of Object.entries(r)) {
      assert.equal(x.failed, 0, `${name}: ${x.failed} mails failed`)
      assert.equal(x.sent, N, `${name}: sent ${x.sent} of ${N}`)
      assert.equal(x.relay, N, `${name}: the relay saw ${x.relay} of ${N}`)
    }
    assert.ok(r.native.drained, 'the native queue drained')

    /* The measured gap is around 14x; anything at or above parity means the
     * native path is not silently broken. */
    const best = Math.min(r.nodemailerDefault.perMail, r.nodemailerNoDelay.perMail)
    assert.ok(
      r.native.perMail < best,
      `sc_mailer ${(r.native.perMail / 1000).toFixed(0)} us vs nodemailer ${(best / 1000).toFixed(0)} us`,
    )
  } finally {
    ca.cleanup()
  }
})
