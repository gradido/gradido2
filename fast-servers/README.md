# fast-servers

The C implementation of the gradido2 servers. One binary, three roles.

Read [AGENTS.md](AGENTS.md) and [Architecture.md](Architecture.md) before changing anything in
here; this file is only how to build and run it.

## Build

```sh
zig build                      # the binary, in zig-out/bin/fast-servers
zig build run -- --federation  # build and run, roles after --
```

`build.zig` is the master build. It fetches and compiles everything it needs, so nothing has to
be installed for it beyond a zig toolchain. That holds without an exception — the TLS library,
zlib and both database drivers included. `ldd zig-out/bin/fast-servers` names `libm` and `libc`
and nothing else.

Options, all with `-D`:

| option | default | what it does |
|---|---|---|
| `h2o` | on, forced off on Windows | build the h2o HTTP backend; off selects the fallback |
| `postgres` | on, forced off on Windows | build the PostgreSQL driver. Off also skips a 155 MB fetch |
| `sqlite` | on | build the SQLite driver |
| `tests` | off | the googletest binaries and the integration probe |
| `benchmarks` | off | the `bench_*` binaries |
| `sanitize` | `off` | `undefined_behavior` (UBSan) or `thread` (TSan) |

AddressSanitizer is not in that list: zig does not ship the asan runtime, so it comes from the
CMake build instead.

On a Debian-style host the build generates the libc description `zig libc` does not — without it
libsodium and libtsan fail on `asm/errno.h`. See `nativeLibcFile` in `build.zig`; `--libc <file>`
overrides it.

Naming the host's own triple works and is the same build: `-Dtarget=x86_64-linux-gnu` on that
machine gets `/usr/include` and `/usr/lib` back, which zig otherwise withholds from anything it
considers a cross build.

Cross compiling needs no options at all, the fast backend included:

```sh
zig build -Dtarget=aarch64-linux-musl    # h2o, LibreSSL, libpq, SQLite. arm64 servers
zig build -Dtarget=x86_64-linux-musl
zig build -Dh2o=false -Dtarget=x86_64-windows-gnu
```

Windows needs `-Dh2o=false`, because h2o is a posix event loop; `-Dpostgres` turns itself off
there. Everything else is the default build.

## h2o, and the fallback behind the same header

h2o is the server:

```text
h2o                    HTTP/1.1 and HTTP/2, an answer in 11.6 µs
libuv+picohttpparser   one thread, one event loop, HTTP/1.1, no TLS
```

Both sit behind `service_core/http.h`, and a role does not know which one answered.
`fast-servers --version` says.

The second one is not an alternative to h2o. It exists because h2o is a posix event loop and
does not compile against the MSVC runtime, which would otherwise leave the Windows build with no
server at all — and one thread means one core, whatever the machine has, so it is not something
to deploy. It is there so that the roles, the configuration and the domain code can be worked on
and debugged where h2o cannot build. The fast path targets the Linux server this project runs
on, and a high-performance Windows build is not on the table.

Its parser comes out of the same pinned h2o checkout h2o itself is compiled from —
`deps/picohttpparser` — so both builds fetch h2o whatever they select. That costs the Windows
build a download it does not compile, and it buys one pinned copy of those two files instead of
a copy in this repository that nothing would ever compare against the original again.

## Tests

```sh
zig build -Dtests test      # the googletest binaries, built and run
```

Unit tests live beside the component they test — `service-core/tests/`, `backend-core/tests/`,
`backend/tests/` — rather than in one tree at the root, which is where arnm and gradido-blockchain-core keep theirs. Those are one
library each; this is five, and a test binary that links one component and sees only that
component's include directory is what proves the header carries its own dependencies. A shared
test tree with all five paths on it can never fail that way.

The cache and mail tests are worth running under `-Dsanitize=thread`. A reference counted
structure under two locks does not fail a single-threaded test when it is wrong, and neither does
a worker pool with a missing lock; both fail in production, under load, weeks later.

Two of the mail tests need someone on the other end and skip without one, because a green test
that sent nothing says nothing:

```sh
../h20Test/smtp_client/sink.py --port 2525 &
SC_MAIL_TEST_URL=smtp://127.0.0.1:2525 ./zig-out/bin/test_mail
```

The maildev container of the root `docker-compose.yml` is the same thing with a web interface
on it, which is easier when the question is what a mail *looks* like rather than whether it was
accepted:

```sh
docker compose up -d maildev
SC_MAIL_TEST_URL=smtp://127.0.0.1:1026 ./zig-out/bin/test_mail
```

`SC_MAIL_SLOW_URL` points at a *deliberately slow* SMTP server — 150 ms per mail or more — and
enables the one test that watches the worker pool grow under a backlog and retire again
afterwards. Any relay that answers slowly will do. `SC_MAIL_TEST_LOG` turns the mailer's log
lines back on while a test is being worked on.

`tests/contract/` runs `contracts/test-vectors/` against this implementation —
`test_jwt_contract` is the first, and `packages/contract-tests` runs the same file against the
TypeScript path. Neither side is the authority; the file is, and a disagreement between the two
implementations shows up as one named vector rather than as two suites that are both green.
`AGENTS.md` section 7 has where a test goes and why.

`tests/integration/` drives the built binary over raw sockets from `bun test`, once against each
HTTP backend — see [its README](tests/integration/README.md), which also lists where the two
backends genuinely differ.

```sh
zig build -Dbenchmarks --release=fast && ./zig-out/bin/bench_jwt
```

`benchmarks/` is built the way the server is and linked against the same libsodium, which is the
point: what a JWT costs depends on which SHA-256 is underneath, and the one this build pins is
not the one on the system. A Debug build measures Debug — pass `--release=fast` for a number
worth quoting, and name the machine beside it.

## Run

```text
fast-servers                        the backend, which is the default
fast-servers --federation           federation only
fast-servers --backend --dht-node   both, in one process, one thread each
fast-servers --help
```

Several roles in one process share one backend-core and one log stream. Splitting them across
processes needs no code change — it is three invocations.

Configuration comes from the environment, with legacy's names and ports:

| variable | default |
|---|---|
| `LISTEN_HOST` | `127.0.0.1` |
| `BACKEND_PORT` | `4000` |
| `FEDERATION_PORT` | `5010` |
| `DHT_PORT` | `5000` |
| `FEDERATION_DHT_TOPIC` | unset — `--dht-node` refuses to start without it |
| `FEDERATION_DHT_SEED` | unset |
| `LOG_LEVEL` | `info` |
| `NODE_ENV` | `development` | `production` refuses an empty `DB_PASSWORD` on PostgreSQL, answers cross-origin requests from loopback only, and makes `migrate-down` ask for a confirmation |

Each role answers `GET /_health`. Beyond that the backend serves one contracted route,
`POST /user/create` — registration, and the first of the 139 in `contracts/server/`. Every other
path on the backend answers `ROUTE_NOT_IMPLEMENTED` (501) rather than 404, because a deployment
runs one implementation and never forwards to the other. Federation serves nothing yet, and peer
discovery is a stub that finds nobody.

```sh
fast-servers migrate-down          # take the database down one migration, then stop
```

`migrate-down` is a command rather than a role: a serving start migrates *up* to the version its
code needs, so taking the database to N-1 and then serving a build that needs N would undo the
step and re-apply it in the same breath. In development it runs; on a release
(`NODE_ENV=production`) it runs only when `DB_MIGRATE_DOWN` names the migration one lower — the
version the database is to end at, or `0` for an empty one.

Who may call the backend from a browser is `backend/src/cors.c`, and it is the same policy the
reference path configures: credentials allowed, `GET POST PUT DELETE`, any origin in development
and loopback alone in production — a deployment serves the frontend from the backend's own
origin, so in production a browser has no cross-origin question to ask. The three headers that
file deliberately does not copy from `@elysiajs/cors`, and what a browser does with each of them,
are written down at the top of it.

## The database

Both drivers are in the binary and which database is used is read at startup, never built in.
`fast-servers --version` says which ones this build has.

```text
libpq      compiled from a pinned postgres checkout; -Dpostgres=false leaves it out.
           Not on Windows: that package compiles postgres' posix src/port and claims
           Linux and macOS only, so `zig build` there refuses rather than letting the
           compiler explain it. A Windows build reaches an installed libpq through
           CMakeLists.txt, which is the file that exists for that target.
sqlite     compiled from sqlite.org's amalgamation; -Dsqlite=false leaves it out.
           Builds everywhere, Windows included.
```

libpq is built against the same LibreSSL h2o uses, so a database on another machine is reached
over TLS. On the same machine, point `DB_HOST` at the socket directory instead — it is 83.4 →
48.1 µs for one connection string.

> [!WARNING]
> The repository root has a `docker-compose.yml` with a PostgreSQL for development. Do not
> benchmark against it. A published container port replaces that socket with TCP through a
> network namespace and NAT — the 48.1 µs above is not reachable from it — and on Windows and
> macOS the packet crosses a virtual machine on top. Feature work against it is fine; a number
> out of it says something about docker. See *Development containers* in the root README.md.

The variables are the ones the TypeScript path reads, with the same defaults:

| variable | default | |
|---|---|---|
| `DB_TYPE` | `postgresql` | or `sqlite` |
| `DB_HOST` | `localhost` | a value starting with `/` is a Unix socket directory, which is what a database on this host should be reached through — 83.4 → 48.1 µs |
| `DB_PORT` | `5432` | |
| `DB_USER` | `gradido` | |
| `DB_PASSWORD` | empty | refused when empty and `NODE_ENV=production`, as on the TypeScript path |
| `DB_DATABASE` | `gradido_community` | |
| `DB_FILE` | `./gradido_community.sqlite` | SQLite only |

**The backend opens one at startup and refuses to serve without it.** It waits for the database,
migrates it to the version this build carries, and reads — or asks for — the community this
instance is; `backend-core/src/context.c` is that sequence and each of its failures is one log
line. The migrations are `contracts/migrations/`, embedded in the binary by the build, and the
schema they build is the one the TypeScript path builds from the same files.

What is still missing — the asynchronous PostgreSQL path and the generated row mapping — is in
[Architecture.md](Architecture.md), *Databases*. Both statements a repository sends today are
written by hand against the driver, which is what that section prescribes until the generator
exists.

`test_db` covers the configuration, the refusals and SQLite for real. Its two PostgreSQL tests
need a server and skip without one, because a green test that connected to nothing says
nothing:

```sh
SC_DB_TEST_PG_HOST=/var/run/postgresql SC_DB_TEST_PG_DATABASE=gradido \
    ./zig-out/bin/test_db
```

## The CMake build

`CMakeLists.txt` exists for the MSVC ABI, which zig cannot target because it does not ship the
Windows SDK. It mirrors `build.zig` and never leads it.

```sh
cmake -B build && cmake --build build
cmake -B build -DFS_ENABLE_TESTS=ON && ctest --test-dir build
cmake -B build -DFS_ENABLE_H2O=ON              # only where libh2o-evloop is installed
cmake -B build -DFS_ENABLE_POSTGRES=ON         # only where libpq is installed
```

Its dependencies are fetched rather than looked for — the core, libsodium, libuv,
picohttpparser and the SQLite amalgamation included — so a Windows developer needs Visual
Studio and nothing else.

Two things it does not build, and both are why an option that is **on** in `build.zig` is
**off** here:

- **h2o.** Compiling it is a page of `build.zig` that would have to be written a second time in
  CMake, and it does not build for the target this file exists for at all — so this build asks
  pkg-config for an installed `libh2o-evloop` instead, and defaults to not asking. It still
  fetches the h2o checkout either way, for its `deps/picohttpparser`.
- **libpq.** `zig build` compiles it out of a pinned postgres checkout; this one asks
  `find_package(PostgreSQL)`, the way it asks for curl. A Windows developer's machine does not
  normally have libpq, and failing `cmake -B build` out of the box over a driver the zig build
  provides anyway would be the wrong trade. SQLite has no such problem — it is one C file, so
  this build fetches and compiles it exactly as `build.zig` does, and `FS_ENABLE_SQLITE`
  is on.

On Linux the CMake build is worth having for a second reason: it prints the `-Wall -Wextra`
findings. `zig build` hides C compiler warnings when a step succeeds and turns them into errors
when one fails, so a green `zig build` says nothing about them either way.

## What is here

```text
src/main.c        role selection, the quit flag, one thread per role
service-core/     logging, config, the HTTP surface and its two backends,
                  the cache table, JWT, the database connection and its two
                  drivers. Threads and locks come from libuv
service-core/email/
                  the mail half, in three layers: message (the bytes),
                  transport (one SMTP session) and mailer (queue, retry,
                  worker pool). render and templates are the template
                  renderer, and those two are copies -- built out of the pug
                  templates by packages/email-native and written into this
                  tree by that package's build. Change them there, not here
                  -- AGENTS.md section 3c
backend-core/     the backend domain: the migration runner, the repositories and
                  the interactions, under the data/logic/interactions/repositories
                  layout Architecture.md prescribes. Nothing originates here --
                  every line of it is a translation of packages/backend-core
backend/          the HTTP server the frontend talks to
federation/       the HTTP server other communities talk to
dht-node/         the peer discovery role, and the extern "C" boundary to the
                  rust-libp2p module that will sit behind it
tests/contract    contracts/test-vectors/, run against this implementation
tests/integration the probe server and the bun suite that drives it
```
