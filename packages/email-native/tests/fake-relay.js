/*
 * An SMTP sink, in its own process so that it does not share an event loop with
 * whatever is sending at it. Accepts anything, stores nothing, counts messages.
 *
 *   node tests/fake-relay.js                       plain SMTP
 *   node tests/fake-relay.js --key k --cert c      implicit TLS (smtps)
 *
 * Prints "PORT <n>" on stdout once listening, and the message count on stderr
 * when it is asked to stop.
 */
const net = require('node:net')
const tls = require('node:tls')
const fs = require('node:fs')

const arg = (name) => {
  const i = process.argv.indexOf('--' + name)
  return i >= 0 ? process.argv[i + 1] : null
}
const key = arg('key')
const cert = arg('cert')

let messages = 0

function onConnection(socket) {
  socket.setNoDelay(true)
  let inData = false
  let tail = '' // carries the last few bytes, so "\r\n.\r\n" is found across chunks
  let pending = ''

  socket.on('data', (chunk) => {
    let s = chunk.toString('latin1')

    while (s.length) {
      if (inData) {
        const hay = tail + s
        const end = hay.indexOf('\r\n.\r\n')
        if (end === -1) {
          tail = hay.slice(-4)
          s = ''
        } else {
          messages++
          inData = false
          tail = ''
          socket.write('250 2.0.0 Ok\r\n')
          s = hay.slice(end + 5)
        }
        continue
      }

      pending += s
      s = ''
      const lines = pending.split('\r\n')
      pending = lines.pop()
      for (const line of lines) {
        const verb = line.slice(0, 4).toUpperCase()
        if (verb === 'EHLO') {
          socket.write('250-fake\r\n250-PIPELINING\r\n250 8BITMIME\r\n')
        } else if (verb === 'DATA') {
          socket.write('354 End data with <CR><LF>.<CR><LF>\r\n')
          inData = true
          s = pending
          pending = ''
          break
        } else if (verb === 'QUIT') {
          socket.write('221 Bye\r\n')
          socket.end()
        } else {
          socket.write('250 2.0.0 Ok\r\n')
        }
      }
    }
  })

  socket.on('error', () => {
    /* a client that drops mid-session is the test's business, not the relay's */
  })
  socket.write('220 fake ESMTP\r\n')
}

const server =
  key && cert
    ? tls.createServer({ key: fs.readFileSync(key), cert: fs.readFileSync(cert) }, onConnection)
    : net.createServer(onConnection)

server.on('tlsClientError', () => {
  /* the untrusted-CA test makes the client refuse the chain; that is the point */
})

server.listen(0, '127.0.0.1', () => {
  process.stdout.write(`PORT ${server.address().port}\n`)
})

const report = () => {
  process.stderr.write(`RELAY ${messages}\n`)
  process.exit(0)
}
process.on('SIGTERM', report)
process.on('SIGINT', report)
