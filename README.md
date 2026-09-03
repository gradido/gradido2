# Gradido2

A rebuild of gradido legacy on a new stack, with the complete legacy feature set as the
target scope.

TypeScript is the reference implementation (`packages/`), with an additional fast
implementation of the backends in C (`fast-servers/`). Determinism-critical code — money
arithmetic, decay, signing — exists once, in C, and is used by both.

See [Architecture.md](Architecture.md) for the design and [AGENTS.md](AGENTS.md) for the
working rules.

## One binary

```sh
bun bundle                     # build/gradido2 — the TypeScript server
BUNDLE_C=1 bun bundle          # and build/gradido2-fast, the C one, beside it
./build/gradido2               # start it: the backend, and the frontend it serves
./build/gradido2 --help        # the services and commands it has

turbo publish                  # only the pages, for looking at what a binary would carry
```

Either implementation ships as a single executable: the server, the frontends it serves, and
its runtime. Nothing has to be installed next to it — copy the file onto a server, start it,
and it opens a SQLite database beside itself and asks who this community is. That is the
download-and-start promise [Architecture.md](Architecture.md) makes; `scripts/bundle.ts` and
`fast-servers/build.zig` are where it is kept.

| variable | default | what it builds |
|---|---|---|
| `BUNDLE_TS` | `1` | `build/gradido2` — the TypeScript path, the reference implementation |
| `BUNDLE_C` | `0` | `build/gradido2-fast` — the C path. Needs a zig toolchain, and a first build fetches and compiles h2o, LibreSSL and libpq |

> [!IMPORTANT]
> The two are **implementations of the same server, not components of one**. A deployment runs
> one or the other, and never both against one database — they each hold the session cache in
> their own process, and two of them migrating one schema is not a state either can report.
> `AGENTS.md`, *C is the fast implementation*, has the whole rule. Sitting next to each other in
> `build/` makes them easy to compare and easy to confuse; deploy one file.

A plain `zig build` in `fast-servers/` still writes `zig-out/bin/gradido2-fast`, which is where
the C build keeps its own artifact. `build/` is what a release goes into.

**The pages belong to neither and are built once.** `turbo publish` assembles `publish/` — the
built frontends plus a manifest saying where each is mounted and what every file's content type
and ETag are — and both builds embed what it names. Neither server can build a page: a page is
vite's output. `bun bundle` and `zig build` both run it themselves — and both go through
turbo, so building both implementations publishes once and the second ask is a cache hit. It is
generated, so it is gitignored.

| what | where it comes from |
|---|---|
| the backend | `packages/backend`, started through `runBackend` rather than by importing a file that runs |
| the frontend | `publish/frontend`, every file embedded and handed out by `packages/backend/src/server/staticRoutes.ts` — and by `fast-servers/backend/src/static_routes.c` on the other path |
| `shared-native`, `email-native` | the `.node` addons, embedded the same way |
| federation, dht-node, admin | not written yet. `packages/bundle/src/cli.ts` and `scripts/publish.ts` say where each joins |

Configuration is the environment, as it is for a checkout — `packages/backend/.env.dist`
lists what there is, and a `.env` next to the binary is read. The frontend inside it calls
the origin it was served from, so a deployment has no URL to configure and the backend sends
no CORS headers to itself; `bun bundle` is what builds it that way.

**The TypeScript binary is for the platform it was built on.** The embedded addons are machine
code, so there is no cross-compilation flag that would help: build it on the system it will run
on, or on one like it. The C one cross-compiles freely — `fast-servers/README.md` has the
targets — because everything in it is compiled from source by the same toolchain.

Both are named after the product rather than after their directory: `gradido2` is the server,
and `-fast` says which of the two implementations answered when somebody reads `--version` off
a machine six months from now.

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
