import { afterEach, describe, expect, test } from 'bun:test'
import { type AddressInfo, createServer, type Server, type Socket } from 'node:net'
import { probePostgres } from './findPostgres'

/* Short, because two of the cases below are waited out on purpose. */
const TIMEOUT_MS = 200

/** What the probe sent, per server, so a test can say it was the SSLRequest and nothing else. */
const received = new Map<Server, Buffer>()
const servers: Server[] = []
const sockets: Socket[] = []

/**
 * A listener on loopback that answers the first eight bytes it is sent with @p reply — or with
 * nothing at all when there is none, which is a service that accepts and then waits.
 */
async function listener(reply: string | undefined): Promise<{ server: Server; port: number }> {
  const server = createServer((socket) => {
    sockets.push(socket)
    let bytes = Buffer.alloc(0)
    socket.on('data', (chunk: Buffer) => {
      bytes = Buffer.concat([bytes, chunk])
      received.set(server, bytes)
      if (bytes.length >= 8 && reply !== undefined) {
        socket.write(reply)
      }
    })
    socket.on('error', () => {
      /* The probe hangs up once it has its answer; that is the ordinary end here. */
    })
  })
  servers.push(server)
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
  return { server, port: (server.address() as AddressInfo).port }
}

/** A port nothing listens on: one that was just listened on and let go. */
async function closedPort(): Promise<number> {
  const { server, port } = await listener(undefined)
  await new Promise<void>((resolve) => server.close(() => resolve()))
  return port
}

afterEach(async () => {
  for (const socket of sockets.splice(0)) {
    socket.destroy()
  }
  await Promise.all(
    servers
      .splice(0)
      .map((server) => new Promise<void>((resolve) => server.close(() => resolve()))),
  )
  received.clear()
})

describe('probePostgres', () => {
  test.each([
    ['N'],
    ['S'],
  ])('a server that answers the SSLRequest with %s is PostgreSQL', async (reply) => {
    const { port } = await listener(reply)

    expect(await probePostgres('127.0.0.1', port, TIMEOUT_MS)).toBe('postgresql')
  })

  test('sends the SSLRequest and nothing before it', async () => {
    const { server, port } = await listener('N')
    await probePostgres('127.0.0.1', port, TIMEOUT_MS)

    expect([...(received.get(server) ?? [])]).toEqual([0, 0, 0, 8, 0x04, 0xd2, 0x16, 0x2f])
  })

  test('something that answers with anything else is not PostgreSQL', async () => {
    const { port } = await listener('HTTP/1.1 400 Bad Request\r\n\r\n')

    expect(await probePostgres('127.0.0.1', port, TIMEOUT_MS)).toBe('other')
  })

  test('something that accepts and says nothing is not PostgreSQL either', async () => {
    const { port } = await listener(undefined)

    expect(await probePostgres('127.0.0.1', port, TIMEOUT_MS)).toBe('other')
  })

  test('a port nothing listens on is nothing', async () => {
    expect(await probePostgres('127.0.0.1', await closedPort(), TIMEOUT_MS)).toBe('nothing')
  })
})
