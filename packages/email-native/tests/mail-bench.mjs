/*
 * Sending, not rendering: the same pre-rendered message pushed N times through the same SMTP
 * sink, once by the addon and once by nodemailer — plain and over TLS.
 *
 * The relay runs in its own process, so neither client competes with it for an event loop.
 * `CONNECTIONS` means different things to the two: nodemailer pools that many and holds them,
 * the addon opens one connection per mail and that number is how many are in flight. So the
 * addon pays a handshake per mail here and nodemailer does not — which is the trade the addon
 * made when it moved onto the libuv thread pool, and the reason to look at the plain and the
 * smtps rows separately.
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
  console.log(formatRow('addon, one conn per mail', r.native))
  console.log(formatRow('nodemailer', r.nodemailerDefault))
  console.log(formatRow('nodemailer + TCP_NODELAY', r.nodemailerNoDelay))
  const best = Math.max(r.nodemailerDefault.perMail, r.nodemailerNoDelay.perMail)
  const worst = Math.min(r.nodemailerDefault.perMail, r.nodemailerNoDelay.perMail)
  console.log(
    `  -> the addon is ${(worst / r.native.perMail).toFixed(1)}x to ` +
      `${(best / r.native.perMail).toFixed(1)}x nodemailer\n`,
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

    /* What TLS costs each side. For the addon that is a *handshake* per mail and not just
     * record-layer work, because it opens a connection per mail -- so this row is the price of
     * not pooling, measured, and the number to weigh against `fast-servers`, which does pool.
     * nodemailer holds its connections here, which is why its two rows barely differ. */
    const overhead = (a, b) => `${((b.perMail / a.perMail - 1) * 100).toFixed(0)} %`
    console.log('TLS cost on the same client (a handshake per mail for the addon, see the comment):')
    console.log(`  addon       ${overhead(plain.native, secure.native)}`)
    console.log(`  nodemailer  ${overhead(plain.nodemailerNoDelay, secure.nodemailerNoDelay)}`)
  } finally {
    ca.cleanup()
  }
}
