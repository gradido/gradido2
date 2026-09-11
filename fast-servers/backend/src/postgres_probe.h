/*
 * Whether a PostgreSQL server answers on this machine -- what the setup command asks before it
 * asks which database to use, so that the question can say what is already running.
 * `packages/backend/src/setup/findPostgres.ts` is the reference this is held to: the same two
 * ports, the same eight bytes, the same three answers.
 *
 * **Asked the way a client begins, not by whether a port is open.** An open port says that
 * something listens; what listens on 5432 is usually PostgreSQL and is not always. The first
 * thing a PostgreSQL client may send is an SSLRequest -- a length and a code, eight bytes -- and
 * a server answers it with a single 'S' or 'N' before anything else happens: no user, no
 * password, no database named. Nothing else speaks that way, and PostgreSQL logs nothing for a
 * client that hangs up after the answer, because a client that does not like the answer does
 * exactly that.
 *
 * Blocking, on a loop of its own that lives for one call. The setup command runs before any
 * server does, on the process's only thread, so there is no h2o loop here to share and nothing
 * the wait could hold up -- fast-servers/AGENTS.md, *libuv is the platform layer*, is why the
 * socket is libuv's rather than a second platform seam of this file's own.
 */
#ifndef BACKEND_POSTGRES_PROBE_H
#define BACKEND_POSTGRES_PROBE_H

/** The port a PostgreSQL installation listens on unless it was told otherwise. */
#define BK_POSTGRES_DEFAULT_PORT 5432

/** The port the repository's docker-compose.yml publishes its PostgreSQL on -- README.md,
 *  *Development containers*, says why it is not 5432. */
#define BK_POSTGRES_COMPOSE_PORT 15432

/**
 * The address both ports are asked on.
 *
 * An address rather than `localhost`, so that the question is the same one on both paths: what a
 * name resolves to first is the resolver's decision, and the two implementations do not share
 * one. A server on loopback listens on 127.0.0.1 -- the docker port is published on every
 * address, and PostgreSQL's own default listen_addresses is `localhost`, which includes it.
 */
#define BK_POSTGRES_LOOPBACK "127.0.0.1"

/** Loopback answers in microseconds or not at all. A second is for a port that a firewall
 *  swallows rather than refuses, and it is paid once, while somebody waits for a question. */
#define BK_POSTGRES_PROBE_TIMEOUT_MS 1000

/** What a port turned out to be. */
typedef enum bk_postgres_answer {
    /** Refused, or not answered in time: nothing accepts there. */
    BK_POSTGRES_NOTHING = 0,
    /** Accepted, and then said something else or nothing at all. */
    BK_POSTGRES_OTHER,
    /** Answered the SSLRequest. */
    BK_POSTGRES_FOUND
} bk_postgres_answer;

/**
 * Asks @p port on the IPv4 address @p ip whether it is PostgreSQL, waiting at most
 * @p timeout_ms for the answer.
 *
 * Every way this can go is one of the three answers -- an address that does not parse and a
 * loop that cannot be started are BK_POSTGRES_NOTHING, because the caller has nothing to do with
 * either other than report the port as not being PostgreSQL.
 */
bk_postgres_answer bk_postgres_probe(const char *ip, int port, unsigned timeout_ms);

#endif /* BACKEND_POSTGRES_PROBE_H */
