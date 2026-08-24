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
be installed for it beyond a zig toolchain — and, on the h2o path, OpenSSL and zlib, which h2o
links against.

Options, all with `-D`:

| option | default | what it does |
|---|---|---|
| `h2o` | on, forced off on Windows | build the h2o HTTP backend; off selects the fallback |
| `tests` | off | the googletest binaries and the integration probe |
| `sanitize` | `off` | `undefined_behavior` (UBSan) or `thread` (TSan) |

AddressSanitizer is not in that list: zig does not ship the asan runtime, so it comes from the
CMake build instead.

On a Debian-style host the build generates the libc description `zig libc` does not — without it
libsodium and libtsan fail on `asm/errno.h`. See `nativeLibcFile` in `build.zig`; `--libc <file>`
overrides it.

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
and debugged where h2o cannot build. It came from
[`../h20Test/fallback_server`](../../h20Test/fallback_server), where the machinery around the
parser was written and tested against raw sockets — keep-alive, pipelining, chunked bodies
decoded in place, a bounded drain so an error response is not lost to an RST. A high-performance
build for Windows is not on the table anyway: the fast path targets the Linux server this
project runs on.

Its parser comes out of the same pinned h2o checkout h2o itself is compiled from —
`deps/picohttpparser` — so both builds fetch h2o whatever they select. That costs the Windows
build a download it does not compile, and it buys one pinned copy of those two files instead of
a copy in this repository that nothing would ever compare against the original again.

## Tests

```sh
zig build -Dtests test      # the googletest binaries, built and run
```

Unit tests live beside the component they test — `service-core/tests/` — rather than in one
tree at the root, which is where `../arnm` and `../blockchain-core` keep theirs. Those are one
library each; this is five, and a test binary that links one component and sees only that
component's include directory is what proves the header carries its own dependencies. A shared
test tree with all five paths on it can never fail that way.

The cache tests are worth running under `-Dsanitize=thread`. A reference counted structure under
two locks does not fail a single-threaded test when it is wrong; it fails in production, under
load, weeks later.

`tests/integration/` drives the built binary over raw sockets from `bun test`, once against each
HTTP backend — see [its README](tests/integration/README.md), which also lists where the two
backends genuinely differ.

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

Each role answers `GET /_health`. That is the whole of what is implemented: the routes in
`contracts/server/` are not served yet, and peer discovery is a stub that finds nobody.

## The CMake build

`CMakeLists.txt` exists for the MSVC ABI, which zig cannot target because it does not ship the
Windows SDK. It mirrors `build.zig` and never leads it.

```sh
cmake -B build && cmake --build build          # linux, needs libh2o-evloop via pkg-config
cmake -B build -DFS_ENABLE_H2O=OFF             # what the Windows build does automatically
cmake -B build -DFS_ENABLE_TESTS=ON && ctest --test-dir build
```

Its dependencies are fetched rather than looked for — libsodium, libuv and picohttpparser
included — so a Windows developer needs Visual Studio and nothing else. What it does not fetch is
a *buildable* h2o: `FS_ENABLE_H2O=ON` asks pkg-config for a `libh2o-evloop` that is already
installed, because compiling h2o is a page of `build.zig` that would have to be written a second
time here and it does not build for the target this file exists for. The fallback path still
fetches the h2o checkout, for its `deps/picohttpparser` and nothing else.

On Linux the CMake build is worth having for a second reason: it prints the `-Wall -Wextra`
findings. `zig build` hides C compiler warnings when a step succeeds and turns them into errors
when one fails, so a green `zig build` says nothing about them either way.

## What is here

```text
src/main.c        role selection, the quit flag, one thread per role
service-core/     logging, config, threads, the HTTP surface and its two backends,
                  the cache table, JWT
backend-core/     the backend domain. Empty, and the emptiness is the point
backend/          the HTTP server the frontend talks to
federation/       the HTTP server other communities talk to
dht-node/         the peer discovery role, and the extern "C" boundary to the
                  rust-libp2p module that will sit behind it
tests/integration the probe server and the bun suite that drives it
```
