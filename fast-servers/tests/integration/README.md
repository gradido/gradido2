# Integration tests

Drives the built server over raw sockets, once against each HTTP backend.

```sh
# h2o
zig build -Dtests
bun test

# the fallback
zig build -Dtests -Dh2o=false -p build/fallback
FS_HTTP_PROBE=../../build/fallback/bin/http-probe FS_HTTP_PROBE_PORT=17901 bun test
```

`FS_HTTP_PROBE` names the binary (default `../../zig-out/bin/http-probe`) and
`FS_HTTP_PROBE_PORT` the port (default 17899). The suite prints which backend answered, so a
failure names it.

The probe is started with four loops rather than one per core, so that every test in the suite
runs against a server whose connections are spread over several event loops — and does so
identically on a one-core build machine and a sixteen-core one. The fallback clamps that to one
and says so, which is asserted rather than assumed.

Under a sanitizer, point `TSAN_OPTIONS` or `UBSAN_OPTIONS` at a `log_path`: the harness captures
the probe's stderr and only prints it when the probe dies, so a sanitizer report on a passing run
would otherwise be invisible.

```sh
zig build -Dtests -Dsanitize=thread -p build/tsan
FS_HTTP_PROBE=../../build/tsan/bin/http-probe FS_HTTP_PROBE_PORT=17904 \
  TSAN_OPTIONS=log_path=/tmp/tsan.report bun test
```

## Why raw sockets, and why a probe binary

`fetch` would test bun's idea of HTTP. What is interesting here is what happens to the bytes: a
request arriving one character at a time, two requests in one packet, a body that lands after
its head, and the things the parser hands back as an error rather than as a request. None of
that is expressible through a client library, because a client library exists to stop you
expressing it.

The server under test is `probe/http_probe.c`, not `fast-servers`. The suite needs routes that
report what arrived — a body echoed, a header returned, the method as parsed — and `/_health`
answers the same thirty bytes whatever it is sent. Those routes do not belong in backend or
federation: a route that exists so a test can see something is a route an operator can reach.
So the probe is a second consumer of `service_core/http.h`, built only under `-Dtests`, and the
production roles keep the one route they have.

That the probe compiles at all is part of what is being tested: the seam is meant to carry a
server that is not one of the three roles.

## Where the two backends differ

A role is written against `service_core/http.h` and cannot tell the backends apart, so anything
a *client* can tell apart is worth knowing about. These are asserted rather than smoothed over,
so that none of them can change unnoticed.

| | h2o | fallback |
|---|---|---|
| `Content-Length` **and** `Transfer-Encoding` | 200, the encoding wins (RFC 9112) | 400, refused |
| chunked on HTTP/1.0 | 200, decoded anyway | 400, refused |
| a head over the limit | 400, at `H2O_MAX_REQLEN` ≈ 400 KiB | 431, at `MAX_HEAD` = 8 KiB |
| a chunk size declaring more than the body limit | waits for the bytes | 413 at the size line |
| a transfer encoding other than chunked | 400 | 501 |
| a non-numeric `Content-Length` | 400 | 413 |
| a client that never stops sending | keeps taking it | drain bounded at 2 s / 256 KiB |
| noticing a client left mid-request | not until the write fails | at once, the EOF is read |
| `SERVER_THREADS` | as many loops as asked for | clamped to 1, logged |

The disconnect row is the one to read together with `/defer`. h2o stops reading a connection
while a response is pending, so a client that closes after its request is usually not seen until
the answer is written; the fallback keeps reading while a request is deferred and sees the EOF,
so its resume callback is handed a NULL request. Neither is wrong and nothing may depend on
which happens — what both guarantee is that the request stays alive until it is answered, so a
worker's resume always lands somewhere. `answering later` in the suite asserts exactly that
split.

The first row is the one to weigh before either backend goes behind a proxy: it is the classic
request smuggling setup, and the two resolve it differently. Both are defensible — h2o follows
the RFC, the fallback refuses to pick a winner where the proxy in front of it might pick the
other one — but they are not the same, and the difference is not visible from the C side.

Everything else agrees exactly, including `SC_HTTP_MAX_BODY`, which is one number in
`service_core/http.h` for that reason.

## What this is not

Not part of the root `bun install`. This directory is deliberately not a workspace of the root
`package.json`: `bun install` must not need anything under `fast-servers/`, or the fast path
stops being droppable. It has no dependencies of its own either — `bun:test` and `node:net` are
all it uses — so there is nothing to install.
