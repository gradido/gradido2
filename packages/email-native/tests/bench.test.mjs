/*
 * The send benchmark as a test: a short run over TLS, so `npm test` says both clients still
 * deliver everything and roughly how they compare. The numbers to quote come from
 * tests/mail-bench.mjs, which runs longer and does plain SMTP as well.
 *
 * Both clients now open a connection per mail -- the addon since it moved onto
 * napi_create_async_work, nodemailer here only because `pool` is on. So this no longer
 * measures a pooled sender against an unpooled one, and no speed relation is asserted:
 * what must hold is that every mail arrives, over a verified TLS connection, from both.
 * A benchmark that fails the suite because a machine was busy is worse than no benchmark.
 *
 *   N=2000 node --test tests/bench.test.mjs
 */
import test from 'node:test'
import assert from 'node:assert'
import { compare, formatRow, haveOpenssl, makeCert, renderOne } from './lib/mailbench.mjs'

const N = Number(process.env.N ?? 200)
const CONNECTIONS = Number(process.env.CONNECTIONS ?? 4)

test('smtps: the addon and nodemailer both deliver every mail', {
  skip: !haveOpenssl && 'no openssl to make a certificate with',
  timeout: 180_000,
}, async () => {
  const mail = renderOne()
  const ca = makeCert()
  try {
    const r = await compare({ n: N, connections: CONNECTIONS, ca, mail })

    console.log(`\n  ${N} mails over smtps, ${mail.html.length} B body, ${CONNECTIONS} connections`)
    console.log(formatRow('addon, one conn per mail', r.native))
    console.log(formatRow('nodemailer', r.nodemailerDefault))
    console.log(formatRow('nodemailer + TCP_NODELAY', r.nodemailerNoDelay))

    for (const [name, x] of Object.entries(r)) {
      assert.equal(x.failed, 0, `${name}: ${x.failed} mails failed`)
      assert.equal(x.sent, N, `${name}: sent ${x.sent} of ${N}`)
      assert.equal(x.relay, N, `${name}: the relay saw ${x.relay} of ${N}`)
    }
    assert.ok(r.native.drained, 'every send settled without a rejection')
  } finally {
    ca.cleanup()
  }
})
