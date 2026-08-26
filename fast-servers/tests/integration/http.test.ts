/**
 * The HTTP surface, driven over raw sockets against the built binary.
 *
 * Translated from the raw-socket suite of the h2o prototype that preceded this repository,
 * where every one of these was a bug first. What is new is that this runs against *both* backends:
 * the same file, the same bytes, once against h2o and once against libuv+picohttpparser. A role
 * is written against service_core/http.h and cannot tell them apart, so anything a client can
 * tell apart is a defect in one of the two.
 *
 *   zig build -Dtests                                   # h2o, into zig-out/bin
 *   bun test
 *   zig build -Dtests -Dh2o=false -p build/fallback     # and again with the other one
 *   FS_HTTP_PROBE=build/fallback/bin/http-probe bun test
 *
 * Where the two genuinely differ, the difference is asserted rather than smoothed over, and the
 * test says which backend does what. Those are the places to look first when one of them is
 * replaced.
 */
import { afterAll, beforeAll, describe, expect, test } from 'bun:test'

import { type Probe, startProbe } from './probe'
import { bodyOf, chunked, everyByte, headOf, Raw, responseCount, statusOf } from './raw'

let probe: Probe
/** True for h2o, false for the fallback. A handful of limits are its own rather than ours. */
let isH2o = false

beforeAll(async () => {
  probe = await startProbe()
  isH2o = probe.backend === 'h2o'
  // biome-ignore lint/suspicious/noConsole: a failure has to name which backend produced it
  console.log(`http backend under test: ${probe.backend}`)
})

afterAll(async () => {
  await probe?.stop()
})

const connect = () => Raw.connect(probe.port)

/** One request, `Connection: close`, everything the server said back. */
async function once(request: Buffer | string): Promise<Buffer> {
  const socket = await connect()
  socket.send(request)
  const response = await socket.readAll()
  socket.close()
  return response
}

function get(path: string, extraHeaders = ''): string {
  return `GET ${path} HTTP/1.1\r\nHost: x\r\n${extraHeaders}Connection: close\r\n\r\n`
}

describe('the ordinary path', () => {
  test('GET /hello answers 200 and the body', async () => {
    const response = await once(get('/hello'))
    expect(statusOf(response)).toBe(200)
    expect(bodyOf(response).toString()).toBe('Hello World\n')
  })

  test('an unregistered path is 404', async () => {
    expect(statusOf(await once(get('/nope')))).toBe(404)
  })

  test('a header value survives colons and spaces', async () => {
    const response = await once(get('/echo-header', 'X-Test: a value: with colons, and spaces\r\n'))
    expect(bodyOf(response).toString()).toBe('a value: with colons, and spaces')
  })

  test('header lookup is case-insensitive', async () => {
    const response = await once(get('/echo-header', 'x-TeSt: mixed\r\n'))
    expect(bodyOf(response).toString()).toBe('mixed')
  })

  test('the query string is not part of the path a route sees', async () => {
    const response = await once(get('/path?a=1&b=2'))
    expect(statusOf(response)).toBe(200)
    expect(bodyOf(response).toString()).toBe('/path')
  })

  test('neither backend announces what it is', async () => {
    // h2o sends `Server: h2o/2.3.0-DEV` unless its server_name is emptied, and the fallback has
    // never sent one -- a difference a client can see, and one this suite did not catch until
    // someone read a curl dump. Error responses go through the same header flattening, so both
    // are checked: a 404 is the one an unauthenticated scanner reaches first.
    for (const path of ['/hello', '/nope']) {
      const head = headOf(await once(get(path))).toLowerCase()
      expect(head).not.toContain('server:')
    }
  })

  test('a handler that declines leaves the request unrouted', async () => {
    // /hello answers GET and returns -1 for anything else, which is the convention both
    // backends implement: nothing else is registered for that path, so it is a 404.
    const response = await once(
      `POST /hello HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n`,
    )
    expect(statusOf(response)).toBe(404)
  })
})

describe('the body, which the parser does not read for you', () => {
  test('a POST body comes back whole', async () => {
    const body = 'the quick brown fox'
    const response = await once(
      `POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: ${body.length}\r\nConnection: close\r\n\r\n${body}`,
    )
    expect(bodyOf(response).toString()).toBe(body)
  })

  test('a body arriving after its head, in pieces', async () => {
    const body = Buffer.alloc(4096, 'x')
    const socket = await connect()
    socket.send(
      `POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: ${body.length}\r\nConnection: close\r\n\r\n`,
    )
    await Bun.sleep(50)
    socket.send(body.subarray(0, 1000))
    await Bun.sleep(50)
    socket.send(body.subarray(1000))
    const response = await socket.readAll()
    socket.close()
    expect(bodyOf(response).length).toBe(body.length)
    expect(bodyOf(response).equals(body)).toBe(true)
  })

  test('Content-Length: 0', async () => {
    const response = await once(
      'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n',
    )
    expect(statusOf(response)).toBe(200)
    expect(bodyOf(response).length).toBe(0)
  })

  test('a binary body with NUL bytes survives', async () => {
    const body = everyByte(8)
    const socket = await connect()
    socket.send(
      `POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: ${body.length}\r\nConnection: close\r\n\r\n`,
    )
    socket.send(body)
    const response = await socket.readAll()
    socket.close()
    expect(bodyOf(response).equals(body)).toBe(true)
  })

  test('method, version and body length as parsed', async () => {
    const response = await once(
      'PUT /whoami HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc',
    )
    expect(bodyOf(response).toString()).toBe('PUT HTTP/1.1 body=3\n')
  })
})

describe('incremental arrival', () => {
  test('a request delivered one byte at a time', async () => {
    // This is what the parser's last_len argument exists for: passing 0 every time works and
    // rescans the whole head on every read, which is quadratic in exactly the case an
    // attacker controls.
    const request = Buffer.from(get('/hello'), 'latin1')
    const socket = await connect()
    for (const byte of request) {
      socket.send(Buffer.from([byte]))
    }
    const response = await socket.readAll()
    socket.close()
    expect(statusOf(response)).toBe(200)
    expect(bodyOf(response).toString()).toBe('Hello World\n')
  })

  test('a split inside a header name', async () => {
    const socket = await connect()
    socket.send('GET /echo-header HTTP/1.1\r\nHost: x\r\nX-Te')
    await Bun.sleep(50)
    socket.send('st: split\r\nConnection: close\r\n\r\n')
    const response = await socket.readAll()
    socket.close()
    expect(bodyOf(response).toString()).toBe('split')
  })
})

describe('more than one request on one connection', () => {
  test('keep-alive: two requests, one connection', async () => {
    const socket = await connect()
    socket.send('GET /hello HTTP/1.1\r\nHost: x\r\n\r\n')
    const first = await socket.readOne()
    socket.send(get('/nope'))
    const second = await socket.readOne()
    socket.close()
    expect(statusOf(first)).toBe(200)
    expect(statusOf(second)).toBe(404)
  })

  test('pipelined: two requests in one packet', async () => {
    // The second request is in the buffer before the first has been answered. This is where
    // a missing consume() shows up, and with one request per connection it never would.
    const response = await once(`GET /hello HTTP/1.1\r\nHost: x\r\n\r\n${get('/nope')}`)
    expect(responseCount(response)).toBe(2)
    expect(response.toString('latin1')).toContain('Hello World')
    expect(response.toString('latin1')).toContain(' 404 ')
  })

  test('pipelined with bodies: the framing does not slide', async () => {
    const response = await once(
      'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nfirst' +
        'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsecond',
    )
    expect(responseCount(response)).toBe(2)
    expect(response.toString('latin1')).toContain('first')
    expect(response.toString('latin1')).toContain('second')
  })

  test('HTTP/1.0 closes by default', async () => {
    const response = await once('GET /hello HTTP/1.0\r\nHost: x\r\n\r\n')
    expect(statusOf(response)).toBe(200)
    expect(headOf(response).toLowerCase()).toContain('close')
  })
})

describe('what the parser rejects', () => {
  test('garbage is refused and the connection ends', async () => {
    const response = await once('this is not http at all\r\n\r\n')
    expect(statusOf(response)).toBe(400)
  })

  test('a non-numeric Content-Length is refused', async () => {
    const response = await once('POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n')
    // The fallback answers 413 from one length check; h2o rejects the head itself with 400.
    // Both refuse, neither guesses, and which number comes back is the backend's own.
    expect(statusOf(response)).toBe(isH2o ? 400 : 413)
  })

  test('a body over the limit is refused with 413', async () => {
    const response = await once(
      'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999\r\n\r\n',
    )
    // SC_HTTP_MAX_BODY is the same number in both, which is why this one agrees.
    expect(statusOf(response)).toBe(413)
  })

  test('a head over the limit is refused, and the refusal reaches the client', async () => {
    // The natural way to write it, and the one the fallback could not pass before its
    // graceful shutdown existed: the error went out, the close reset a connection with bytes
    // still arriving, and the reset took the answer with it. The client saw no status code.
    //
    // The two backends draw the line in very different places -- the fallback at MAX_HEAD,
    // 8 KiB, h2o at its compile-time H2O_MAX_REQLEN of a little over 400 KiB -- so the flood
    // has to be large enough to pass both. What is checked is not where the limit sits but
    // that crossing it produces an answer rather than a silent reset.
    const socket = await connect()
    socket.send('GET /hello HTTP/1.1\r\nHost: x\r\n')
    const header = Buffer.from(`X-Pad: ${'y'.repeat(48)}\r\n`, 'latin1')
    await socket.flood(header, 600 * 1024, 10000)
    const response = await socket.readAll()
    socket.close()
    expect(statusOf(response)).toBe(isH2o ? 400 : 431)
  }, 30000)

  test('the refusal survives a client that keeps sending', async () => {
    const socket = await connect()
    socket.send('POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999\r\n\r\n')
    await socket.flood(Buffer.alloc(8192, 'z'), 512 * 1024, 5000)
    const response = await socket.readAll()
    socket.close()
    expect(statusOf(response)).toBe(413)
  }, 30000)

  test('a connection that is talked at forever is let go', async () => {
    // Politeness towards a client that never stops talking is indistinguishable from a leak,
    // so the fallback bounds its drain at DRAIN_MS and DRAIN_MAX -- 2 s and 256 KiB. Past
    // either, the connection goes anyway.
    //
    // h2o has no equivalent bound this test can see, and keeps taking the bytes. So what is
    // asserted for both is the property that actually matters: the server is unharmed
    // afterwards. The byte budget is asserted only where there is one.
    const socket = await connect()
    socket.send(get('/hello'))
    const sent = await socket.flood(Buffer.alloc(16384, 'q'), 4 * 1024 * 1024, 5000)
    socket.close()

    if (!isH2o) {
      expect(sent).toBeLessThan(4 * 1024 * 1024)
    }
    expect(statusOf(await once(get('/hello')))).toBe(200)
  }, 30000)

  test('a client disconnecting mid-request does not take the server with it', async () => {
    const dying = await connect()
    dying.send('GET /hel')
    dying.close()
    await Bun.sleep(50)

    expect(statusOf(await once(get('/hello')))).toBe(200)
  })
})

describe('chunked transfer encoding', () => {
  test('a chunked body is reassembled', async () => {
    const response = await once(
      Buffer.concat([
        Buffer.from(
          'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n',
          'latin1',
        ),
        chunked(['part1', 'part2']),
      ]),
    )
    expect(bodyOf(response).toString()).toBe('part1part2')
  })

  test('500 chunks', async () => {
    const parts = Array.from({ length: 500 }, (_, i) => String(i).padStart(4, '0'))
    const response = await once(
      Buffer.concat([
        Buffer.from(
          'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n',
          'latin1',
        ),
        chunked(parts),
      ]),
    )
    expect(bodyOf(response).toString()).toBe(parts.join(''))
  })

  test('chunked delivered one byte at a time', async () => {
    // Every state boundary is crossed mid-read, which is what the four-state machine has to
    // survive. It is decoded in place, over the wire form, so a slip here corrupts the body
    // rather than failing.
    const request = Buffer.concat([
      Buffer.from(
        'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n',
        'latin1',
      ),
      chunked(['abc', 'defgh']),
    ])
    const socket = await connect()
    for (const byte of request) {
      socket.send(Buffer.from([byte]))
    }
    const response = await socket.readAll()
    socket.close()
    expect(bodyOf(response).toString()).toBe('abcdefgh')
  })

  test('a chunk extension is ignored and the payload kept', async () => {
    const response = await once(
      'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n' +
        '3;name=value\r\nabc\r\n0\r\n\r\n',
    )
    expect(bodyOf(response).toString()).toBe('abc')
  })

  test('a trailer is consumed, not mistaken for the next request', async () => {
    const response = await once(
      Buffer.concat([
        Buffer.from(
          'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n',
          'latin1',
        ),
        chunked(['xy'], 'X-Sum: 1\r\n'),
      ]),
    )
    expect(statusOf(response)).toBe(200)
    expect(bodyOf(response).toString()).toBe('xy')
  })

  test('chunked binary with NUL and CRLF inside', async () => {
    const blob = everyByte()
    const response = await once(
      Buffer.concat([
        Buffer.from(
          'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n',
          'latin1',
        ),
        chunked([blob, blob]),
      ]),
    )
    expect(bodyOf(response).equals(Buffer.concat([blob, blob]))).toBe(true)
  })

  test('a plain request pipelined after a chunked one', async () => {
    // The framing has to end exactly at the terminating chunk, or the next request is parsed
    // out of the trailer.
    const response = await once(
      Buffer.concat([
        Buffer.from(
          'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n',
          'latin1',
        ),
        chunked(['one']),
        Buffer.from(get('/hello'), 'latin1'),
      ]),
    )
    expect(responseCount(response)).toBe(2)
    expect(response.toString('latin1')).toContain('one')
    expect(response.toString('latin1')).toContain('Hello World')
  })

  test('a chunked body over the limit is refused with 413', async () => {
    // SC_HTTP_MAX_BODY is 32 KiB in both backends, so this is one of the places they agree
    // exactly -- the limit is shared on purpose.
    const parts = Array.from({ length: 12 }, () => Buffer.alloc(4096, 'a'))
    const response = await once(
      Buffer.concat([
        Buffer.from(
          'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n',
          'latin1',
        ),
        chunked(parts),
      ]),
    )
    expect(statusOf(response)).toBe(413)
  })

  test('an oversized chunk size is refused before its bytes arrive', async () => {
    // A divergence, and the fallback is the stricter one: it checks the announced size at
    // the size line and answers 413 without waiting. h2o counts what actually arrives, so a
    // chunk header claiming 16 MB followed by nothing leaves it waiting for the body. The
    // test below asserts what each does rather than pretending they agree; if h2o ever
    // starts refusing early, this is where it shows.
    const socket = await connect()
    socket.send(
      'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\nffffff\r\n',
    )
    const response = await socket.readAll(1000)
    socket.close()
    expect(statusOf(response)).toBe(isH2o ? -1 : 413)
  })

  test('a non-hex chunk size is refused with 400', async () => {
    const response = await once(
      'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nabc\r\n0\r\n\r\n',
    )
    expect(statusOf(response)).toBe(400)
  })

  test('Content-Length together with Transfer-Encoding', async () => {
    // The classic request smuggling setup: two headers that say different things about where
    // the request ends, so a proxy and the server behind it can be made to disagree.
    //
    // The two backends answer differently and both are defensible. RFC 9112 says the
    // transfer encoding wins, which is what h2o does -- it serves the request, 200. The
    // fallback refuses with 400 instead, on the grounds that a server picking a winner where
    // the proxy in front of it might pick the other one is exactly the mechanism; it is
    // nobody's front end and has nothing to lose by saying no.
    //
    // This is the divergence to weigh before putting either behind a proxy. It is asserted
    // rather than smoothed over so that it cannot change unnoticed.
    const response = await once(
      Buffer.concat([
        Buffer.from(
          'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n',
          'latin1',
        ),
        chunked(['abc']),
      ]),
    )
    expect(statusOf(response)).toBe(isH2o ? 200 : 400)
  })

  test('a transfer encoding other than chunked is refused', async () => {
    const response = await once('POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n')
    // Both refuse; the number is each backend's own. The fallback says 501, which is the
    // honest answer to an encoding it does not decode, and h2o rejects the framing with 400.
    expect(statusOf(response)).toBe(isH2o ? 400 : 501)
  })

  test('chunked on HTTP/1.0', async () => {
    // Chunked transfer encoding was introduced in HTTP/1.1 and a 1.0 client cannot have
    // meant it. The fallback says 400; h2o decodes it anyway and answers 200. Another
    // divergence where the fallback is the stricter of the two.
    const response = await once(
      Buffer.concat([
        Buffer.from(
          'POST /echo HTTP/1.0\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n',
          'latin1',
        ),
        chunked(['abc']),
      ]),
    )
    expect(statusOf(response)).toBe(isH2o ? 200 : 400)
  })
})

describe('answering later', () => {
  /** `/defer` hands the request to a worker thread, which sends the ticket back after a wait.
   *  What is being tested is that the answer arrives on the loop that owns the request. */
  function deferred(body: string, delayMs: number): string {
    return (
      `POST /defer HTTP/1.1\r\nHost: x\r\nX-Defer-Ms: ${delayMs}\r\n` +
      `Content-Length: ${Buffer.byteLength(body)}\r\nConnection: close\r\n\r\n${body}`
    )
  }

  async function stats(): Promise<{ answered: number; abandoned: number; threads: number }> {
    return JSON.parse(bodyOf(await once(get('/defer-stats'))).toString())
  }

  test('a request answered from another thread comes back intact', async () => {
    const response = await once(deferred('carried across a thread', 30))
    expect(statusOf(response)).toBe(200)
    expect(bodyOf(response).toString()).toBe('carried across a thread')
  })

  test('a deferred answer is framed like any other', async () => {
    // The deferred path sends through h2o_send rather than h2o_send_inline, which is exactly
    // the call that once produced a chunked response instead of a Content-Length one. Neither
    // backend may take that route without the framing following it.
    const response = await once(deferred('framed', 10))
    expect(headOf(response)).toContain('Content-Length: 6')
    expect(headOf(response)).not.toContain('chunked')
  })

  test('an immediate resume is answered too', async () => {
    // Zero delay is the race worth having: the worker may resume before the handler has even
    // returned, so the loop is asked to deliver a ticket for a request it is still dispatching.
    const responses = await Promise.all(
      Array.from({ length: 16 }, (_, i) => once(deferred(`now-${i}`, 0))),
    )
    responses.forEach((response, i) => {
      expect(statusOf(response)).toBe(200)
      expect(bodyOf(response).toString()).toBe(`now-${i}`)
    })
  })

  test('many at once are each answered with their own body', async () => {
    // With several loops these land on different threads, and every one of them has to get its
    // own answer back rather than somebody else's -- which is what the ticket exists to decide.
    const count = 48
    const responses = await Promise.all(
      Array.from({ length: count }, (_, i) => once(deferred(`req-${i}`, 20))),
    )
    responses.forEach((response, i) => {
      expect(statusOf(response)).toBe(200)
      expect(bodyOf(response).toString()).toBe(`req-${i}`)
    })
  })

  test('a client that leaves mid-work does not take the answer with it', async () => {
    const before = await stats()

    const socket = await connect()
    socket.send(deferred('nobody-is-listening', 250))
    await Bun.sleep(30)
    socket.close()

    await Bun.sleep(500)
    const after = await stats()

    // Which of the two counters moved is each backend's own and is asserted below. What must
    // hold for both is that the work finished and was accounted for exactly once: a resume
    // that reached nobody is still a resume that has to be delivered, or its slot leaks.
    expect(after.answered + after.abandoned).toBe(before.answered + before.abandoned + 1)

    if (isH2o) {
      // h2o stops reading a connection while a response is pending, so a client that closes
      // after its request is not noticed until the write fails. The reply is produced and goes
      // nowhere. The generator's `stop` is still what covers the cases h2o does see -- an
      // HTTP/2 reset, a connection error, shutdown -- and it is why the request cannot be
      // disposed under the worker's feet.
      expect(after.abandoned).toBe(before.abandoned)
    } else {
      // The fallback keeps reading while a request is deferred, so it sees the EOF and the
      // resume callback is handed a NULL request.
      expect(after.abandoned).toBe(before.abandoned + 1)
    }

    // And the server is still there, which is the half that matters either way.
    expect(statusOf(await once(get('/hello')))).toBe(200)
  })

  test('the probe runs the loops it was asked for', async () => {
    // Four is what probe.ts asks for. h2o takes it; the fallback is one thread whatever it is
    // told, and logs that it clamped -- AGENTS.md section 3b, it is not a deployment option.
    expect(probe.threads).toBe(isH2o ? 4 : 1)
    expect((await stats()).threads).toBe(probe.threads)
  })
})
