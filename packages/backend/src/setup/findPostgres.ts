import { connect } from 'node:net'

/**
 * Whether a PostgreSQL server answers on this machine — what `setup` asks before it asks which
 * database to use, so that the question can say what is already running.
 *
 * Two ports and no more: the one every PostgreSQL installation listens on unless told otherwise,
 * and the one the repository's `docker-compose.yml` publishes its container on. A server
 * anywhere else is one somebody configured, and whoever did can type its address.
 *
 * **Asked the way a client begins, not by whether a port is open.** An open port says that
 * something listens; what listens on 5432 is usually PostgreSQL and is not always. The first
 * thing a PostgreSQL client may send is an `SSLRequest` — eight bytes, a length and a code — and
 * a server answers it with a single `S` or `N` before anything else happens: no user, no
 * password, no database named. Nothing else speaks that way, and PostgreSQL logs nothing for a
 * client that hangs up after the answer, because a client that does not like the answer does
 * exactly that. `fast-servers/backend/src/postgres_probe.c` asks the same eight bytes.
 */

/** The port a PostgreSQL installation listens on unless it was told otherwise. */
export const POSTGRES_DEFAULT_PORT = 5432

/** The port `docker-compose.yml` publishes its PostgreSQL on — `README.md`, *Development
 *  containers*, says why it is not 5432. */
export const DOCKER_COMPOSE_POSTGRES_PORT = 15432

/**
 * The address both ports are asked on.
 *
 * An address rather than `localhost`, so that the question is the same one on both paths: what
 * a name resolves to first is the resolver's decision, and the two implementations do not share
 * one. A server on loopback listens on 127.0.0.1 — the docker port is published on every
 * address, and PostgreSQL's own default `listen_addresses` is `localhost`, which includes it.
 */
const LOOPBACK = '127.0.0.1'

/** Loopback answers in microseconds or not at all. A second is for a port that a firewall
 *  swallows rather than refuses, and it is paid once, while somebody waits for a question. */
export const PROBE_TIMEOUT_MS = 1000

/** Length 8, then the code 80877103 — 1234 in the high half, 5679 in the low one. */
const SSL_REQUEST = Uint8Array.of(0x00, 0x00, 0x00, 0x08, 0x04, 0xd2, 0x16, 0x2f)

/** What a port turned out to be: PostgreSQL, something else, or nothing that accepts. */
export type PostgresAnswer = 'postgresql' | 'other' | 'nothing'

/** One port that was asked, and what it said. */
export type PostgresPort = {
  readonly port: number
  readonly answer: PostgresAnswer
}

/**
 * Asks @p port on @p host whether it is PostgreSQL.
 *
 * Never rejects: every way this can go is one of the three answers, because the caller has
 * nothing to do with an exception other than report the port as not being PostgreSQL. A
 * connection that was accepted and then said nothing, or said something else, is `other`; one
 * that was refused or never answered is `nothing`.
 */
export function probePostgres(
  host: string,
  port: number,
  timeoutMs = PROBE_TIMEOUT_MS,
): Promise<PostgresAnswer> {
  return new Promise<PostgresAnswer>((resolve) => {
    let connected = false
    let settled = false
    const socket = connect({ host, port })

    const settle = (answer: PostgresAnswer): void => {
      if (settled) {
        return
      }
      settled = true
      socket.destroy()
      resolve(answer)
    }
    const unanswered = (): void => settle(connected ? 'other' : 'nothing')

    socket.setTimeout(timeoutMs, unanswered)
    socket.once('connect', () => {
      connected = true
      socket.write(SSL_REQUEST)
    })
    socket.once('data', (chunk: Buffer) => {
      /* 'S' — it would switch to TLS now — or 'N' — it will go on in plain text. Either one is
         a server that understood the request, and nothing here goes any further with it. */
      settle(chunk[0] === 0x53 || chunk[0] === 0x4e ? 'postgresql' : 'other')
    })
    socket.once('error', unanswered)
    socket.once('close', unanswered)
  })
}

/** Both ports, asked at the same time, in the order they are reported. */
export async function findPostgres(timeoutMs = PROBE_TIMEOUT_MS): Promise<PostgresPort[]> {
  const ports = [POSTGRES_DEFAULT_PORT, DOCKER_COMPOSE_POSTGRES_PORT]
  const answers = await Promise.all(ports.map((port) => probePostgres(LOOPBACK, port, timeoutMs)))
  return ports.map((port, at) => ({ port, answer: answers[at] ?? 'nothing' }))
}
