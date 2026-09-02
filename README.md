# Gradido2

A rebuild of gradido legacy on a new stack, with the complete legacy feature set as the
target scope.

TypeScript is the reference implementation (`packages/`), with an additional fast
implementation of the backends in C (`fast-servers/`). Determinism-critical code — money
arithmetic, decay, signing — exists once, in C, and is used by both.

See [Architecture.md](Architecture.md) for the design and [AGENTS.md](AGENTS.md) for the
working rules.

## Development containers

`docker-compose.yml` starts the three services a developer needs and nothing else: the
reference database, a UI for it, and something that catches mail.

```sh
docker compose up -d              # postgres, adminer and maildev
docker compose up -d postgres     # the database alone
docker compose down               # stop; the database volume survives
docker compose down -v            # stop and delete the database
```

| service | where | what for |
|---|---|---|
| postgres | `localhost:15432` | the reference database. User `gradido`, password `gradido`, database `gradido_community` |
| adminer | http://localhost:8073 | a UI for it. The login *is* the database user above |
| maildev | http://localhost:1081, SMTP `localhost:1026` | catches every mail instead of delivering it |

**Adminer and not pgAdmin**, because `../gradido` puts phpMyAdmin in front of its MariaDB and
someone working on both repositories should have one web UI to learn rather than two. Adminer
is that UI for real rather than by resemblance: one tool that speaks PostgreSQL, MySQL/MariaDB
*and* SQLite — so it covers legacy's MariaDB and this project's PostgreSQL, and SQLite as
well, which is what a gradido2 server uses when nothing is configured. It is also an official
Docker image, which no phpPgAdmin is.

It logs in *as* the database user — no second account the way pgAdmin has one. `gradido` /
`gradido`, database `gradido_community`, and the *Server* field is prefilled with `postgres`.
The one field to set by hand is *System*, which defaults to MySQL: pick PostgreSQL, or open
`http://localhost:8073/?pgsql=postgres&username=gradido&db=gradido_community`, which fills in
everything except the password. That dropdown is the whole cross-database trick — it is how
the same window opens legacy's MariaDB.

For a database *outside* this compose file, name it as `host.docker.internal:3306` (legacy's
MariaDB) or `host.docker.internal:5432` (a native PostgreSQL) in the Server field. That works
under Docker Desktop and rootful Linux docker; under rootless docker the name resolves but
reaches nothing, because the daemon has a network namespace of its own and the host's ports
are not in it. To browse a SQLite file, uncomment the volume in `docker-compose.yml` and give
its path as the Server field.

User, password and database are the defaults of `packages/backend/.env.dist`; the port is
not. **The container publishes 15432, not 5432**, so pointing a server at it is three lines
in `.env`:

```sh
DB_TYPE=postgresql
DB_PORT=15432
DB_PASSWORD=gradido
```

The unusual port is the point. A developer machine that has PostgreSQL installed already
owns 5432, and 5433 is no escape: `pg_createcluster` on Debian and Ubuntu and the EDB
installers hand out 5433, 5434, … for the second and third cluster, so the port that looks
like the obvious fallback is the next one likely to be taken. A container silently occupying
either would also be the wrong kind of convenient — the native server is the one to develop
against when a number matters, see the warning below, and the two should be visibly
different things.

`docker compose` reads the same `.env` the servers do, so `DB_PORT` moves the published port
*and* the server that connects to it and the two cannot drift apart. `DB_USER`,
`DB_PASSWORD`, `DB_DATABASE`, `ADMINER_PORT`, `MAILDEV_WEB_PORT` and `MAILDEV_SMTP_PORT`
work the same way.

The mail sink takes no configuration on the server side yet — the mail tests are pointed at a
relay through their own variables, for example `SC_MAIL_TEST_URL=smtp://127.0.0.1:1026` for
`fast-servers`' `test_mail`.

**No default here collides with `../gradido`.** That repository publishes phpMyAdmin on 8074
and the same maildev image on 1080/1025, so a checkout of legacy and a checkout of gradido2
can be up at the same time without either being reconfigured — which is what the rebuild
needs, since legacy is the behavioral reference and gets looked at while gradido2 is being
written. Everything here therefore sits one above what legacy took: 8073 for Adminer, 1081
and 1026 for maildev. The database has no conflict to avoid — legacy runs MariaDB on 3306 —
but keeps 15432 for the reason above.

> [!WARNING]
> **These containers are for development, not for a deployment, and not for measuring.**
>
> A database in a container is slower than the same database on the host, and the loss lands
> on `fast-servers` hardest, because that is the path whose numbers are small enough to be
> ruined by it. An h2o answer is 11.6 µs; a connection over TCP to a *native* PostgreSQL is
> already 83.4 µs against 48.1 µs over a Unix socket (`fast-servers/README.md`, *The
> database*). A published container port adds a network namespace and NAT on top of that TCP
> path, and the socket the fast path is optimised for cannot be used at all — so a benchmark
> run against this compose file measures docker's networking, not the server.
>
> **On Windows and macOS it is worse, not marginally.** Docker Desktop runs Linux in a
> virtual machine, so every packet and every file access crosses a VM boundary. Expect the
> database to feel slow there in ordinary development, not only under a benchmark.
>
> Use the containers for feature work, where correctness against PostgreSQL is the point. For
> anything with a number in it — the `bench_*` binaries, a latency claim, a comparison between
> the two implementations — run PostgreSQL natively on the host and reach it through
> `DB_HOST=/var/run/postgresql`. For a deployment, see `Architecture.md`; a gradido2 server is
> a binary next to a database, and none of it is built here.
