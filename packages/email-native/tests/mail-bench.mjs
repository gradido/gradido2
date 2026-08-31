/*
 * Sending, not rendering: the same pre-rendered message pushed N times through
 * the same SMTP sink, once by service-core's sc_mailer inside the addon and once
 * by nodemailer — plain and over TLS.
 *
 * The relay runs in its own process, so neither client competes with it for an
 * event loop. Both get the same number of connections, and both hold them: this
 * measures steady-state submission, not handshakes.
 *
 *   node tests/mail-bench.mjs
 *   bun  tests/mail-bench.mjs
 *   N=5000 CONNECTIONS=8 node tests/mail-bench.mjs
 */
import { compare, formatRow, haveOpenssl, makeCert, renderOne } from './lib/mailbench.mjs'

const N = Number(process.env.N ?? 1000)
const CONNECTIONS = Number(process.env.CONNECTIONS ?? 4)

const mail = renderOne()
const runtime =
  typeof globalThis.Bun !== 'undefined' ? `bun ${Bun.version}` : `node ${process.versions.node}`

console.log(
  `${N} mails, ${mail.html.length} B body, ${CONNECTIONS} connections, ` +
    `relay in its own process — ${runtime}\n`,
)

function print(title, r) {
  console.log(title)
  console.log(formatRow('sc_mailer (addon)', r.native))
  console.log(formatRow('nodemailer', r.nodemailerDefault))
  console.log(formatRow('nodemailer + TCP_NODELAY', r.nodemailerNoDelay))
  const best = Math.max(r.nodemailerDefault.perMail, r.nodemailerNoDelay.perMail)
  const worst = Math.min(r.nodemailerDefault.perMail, r.nodemailerNoDelay.perMail)
  console.log(
    `  -> sc_mailer is ${(worst / r.native.perMail).toFixed(1)}x to ` +
      `${(best / r.native.perMail).toFixed(1)}x faster\n`,
  )
}

const plain = await compare({ n: N, connections: CONNECTIONS, ca: null, mail })
print('plain SMTP', plain)

if (!haveOpenssl) {
  console.log('smtps: skipped, no openssl to make a certificate with')
} else {
  const ca = makeCert()
  try {
    const secure = await compare({ n: N, connections: CONNECTIONS, ca, mail })
    print('smtps (implicit TLS, certificate verified)', secure)

    /* What TLS costs each side -- with a caveat that matters more than the
     * number. Connections are held, so this is record-layer work per message
     * rather than a handshake per message; but sc_mailer sits at the relay's
     * ceiling in both rows (it stops scaling at 4 workers: 13.4k mails/s plain,
     * 5.4k over TLS), and nodemailer is nowhere near it. So most of what looks
     * like sc_mailer's TLS cost is the single-process JS relay doing TLS, and
     * both gaps above are floors. */
    const overhead = (a, b) => `${((b.perMail / a.perMail - 1) * 100).toFixed(0)} %`
    console.log('TLS cost on the same client (sc_mailer is relay-bound in both, see the comment):')
    console.log(`  sc_mailer   ${overhead(plain.native, secure.native)}`)
    console.log(`  nodemailer  ${overhead(plain.nodemailerNoDelay, secure.nodemailerNoDelay)}`)
  } finally {
    ca.cleanup()
  }
}
