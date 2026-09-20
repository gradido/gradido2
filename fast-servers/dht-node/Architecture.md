# fast-servers/dht-node Architecture

The network node on the fast path: `rust-libp2p` behind
[`libp2p-ffi`](https://github.com/gradido/libp2p-ffi), prebuilt in that repository and linked
statically behind an `extern "C"` header.

**Read `../../Architecture.md`, *Peer network*, first.** It holds what the node is for, why it is
mirrored rather than shared, how communities and their instances are addressed, the RPC rules and
the discovery design. This file holds the module: why it is Rust, why neither C nor C++ libp2p,
why it is prebuilt, what the prebuild is, and the boundary.

---

## Why Rust, and why not built here

Every other native module in this repository exists because a computation must produce the same
result everywhere, or because a wire format must not be written twice. This one exists because
the library is not complete in C. What the network needs is a routing table with provider records,
plus the transports, the multiplexing, the identify handshake and — because a community without a
URL must be callable — circuit relay. `rust-libp2p` and `js-libp2p` have all of it; the C and C++
implementations do not, see the next section. Completing one is not a module, it is a second
project.

**The module is built to change rarely.** It holds mechanism and nothing else: transports, the
DHT, request and response over opaque bytes, gossipsub for the announcement, relay, rate limiting.
Every policy reaches it from outside — which operations exist, what an envelope looks like, which
class a peer is in, what a node announces, when to walk the DHT. A new RPC operation is a change
in the caller — in Gradido, the federation role — not in this module.

**It is not Gradido's module.** It is a C interface to `rust-libp2p` and speaks of *groups* and
*nodes*, numbered peer classes and opaque payloads — nothing in it knows a community, a URL or a
federation operation. Other projects link it the same way, which is why it is `libp2p-ffi` and not
a gradido repository: the same reason `arnm` left `gradido-blockchain-core`.

That is also what makes a repository of its own the right place. The module is built there,
released there and tested there, and this repository downloads a prebuilt object per platform,
pinned by version and checksum. There is no cargo and no Rust source here, and a developer of the
fast path never needs a Rust toolchain.

```text
repository   github.com/gradido/libp2p-ffi
crate        libp2p-ffi       ->  liblibp2p_ffi.a, shipped localized as libp2p_ffi.o
header       libp2p_ffi.h
prefix       lp2p_
```

Rust is therefore a leaf language in exactly the sense C++ already is: one `extern "C"` surface, no
application logic behind it. `../AGENTS.md`, section 2, holds the rules; they are the C++ ones with
panics in place of exceptions.

---

## Why not c-libp2p or cpp-libp2p

Both exist, both are checked out beside this repository (`../C/c-libp2p`, `../C++/cpp-libp2p`), and
both were the first candidates — a C network node is what this path would prefer. They were
compared once when discovery was the only job, and cpp-libp2p looked sufficient then: without calls
to communities behind NAT, gossip over outbound connections needs no relay. Calling a community
that has no URL is what changed the answer.

```text
                          c-libp2p (Pier Two)      cpp-libp2p (Quadrivium)   rust-libp2p
Kademlia, providers       no                       yes                       yes
circuit relay v2          no                       no (multiaddr code only)  yes
DCUtR / AutoNAT           no / no                  no / no                   yes / yes
gossipsub, Noise, Yamux,  yes                      yes                       yes
QUIC
language, build           C, CMake; OpenSSL,       C++20, vcpkg: Boost,      prebuilt; zig only
                          bison/flex, picoquic     BoringSSL, protobuf,      on this side
                          and seven submodules     lsquic ... 20 packages
size (src + include)      69,700 lines             43,500 lines              -
activity in 2026          63 commits, one author   2 commits                 active
```

What would have to be written, measured against the Rust crates that do it (source including
in-file tests): circuit relay v2, 5,466 lines, for both; Kademlia, 13,670 lines, for c-libp2p as
well; DCUtR, 1,120 lines, later. AutoNAT, 4,457 lines, can be replaced by configuration — a
community with a URL is public, one without reserves a relay. Each of them then has to interoperate
with `js-libp2p`, because the TypeScript path stays on it, and that is the expensive part.

On top of the gaps, neither fits the rules of this path. cpp-libp2p cannot be built by zig, so it
costs cross compilation and the static single binary, and it throws — `../AGENTS.md`, section 2,
compiles C++ modules with `-fno-exceptions`. c-libp2p is the closest in style and the furthest in
function: half of what is needed would be ours to write, in a codebase that allocates per message
at 450 call sites and has a single maintainer.

`rust-libp2p` costs a second language in the release pipeline of another repository and the linker
recipe below. Both are known and measured, which is the difference.

---

## What this module is not

```text
not a database client     it reports what it learns; the federation role persists it
not a domain component    it knows no RPC operation and verifies no envelope
not a config parser       the caller passes an options struct it already filled
not a scheduler           random walks and bootstrap are calls from outside; the node
                          decides only what libp2p itself must (republishing, keep-alive)
not a list of communities a community is looked up by key when it is called
```

Legacy's `dht-node` writes `federated_communities` rows itself. That is the one behavior
deliberately not carried over: a network library in the persistence layer puts persistence
decisions where the rest of the architecture does not look for them.

---

## The prebuild

### A symbol-localized object, not a staticlib

The fast binary is one file with nothing dynamically linked but libc and libm, so the module is
linked statically. A plain Rust `staticlib` does not survive that once there is a second Rust
module — and `gradido-blockchain-zk` is one. Measured with the real zk crate and a libp2p module,
both linked into one C binary with GNU ld and with zig's lld:

```text
two staticlibs, same rustc, no global allocator      links and runs
two staticlibs, rustc 1.85 and 1.90                   duplicate rust_eh_personality -- both
                                                      linkers, both link orders
two staticlibs, one with #[global_allocator]          one link order: duplicate __rust_alloc;
                                                      the other links, and all 51,013
                                                      allocations of grdzk_bundle_init went
                                                      through the libp2p module's allocator
two cdylibs, mixed versions, own allocator            runs, allocators separate
two localized objects, mixed versions, own allocator  runs with both linkers and both orders,
                                                      allocators separate, a panic inside the
                                                      module caught at its boundary
```

Prebuilds come from repositories released on their own schedules, so different rustc versions are
the normal case, not the exception. A `cdylib` works but is a second file beside the binary. What
ships is therefore one relocatable object in which only the `extern "C"` symbols are global:

```sh
ar x liblibp2p_ffi.a                                   # into an empty directory
ld -r *.o -o all.o
objcopy --keep-global-symbols=api.txt -R .group all.o libp2p_ffi.o   # api.txt: the lp2p_ names
```

`-R .group` is not optional: without it lld refuses the link with "relocation refers to a symbol
in a discarded section: `DW.ref.rust_eh_personality`", because both objects carry a COMDAT group
of that name. GNU `nm` crashes on these objects through its LLVM plugin; list symbols with
`readelf -Ws`. The step belongs in the module repository's release pipeline, so what this
repository downloads is already localized.

### Release profile

Measured on the libp2p module (TCP, QUIC, Noise, Yamux, kad, identify) and on the zk crate,
stripped:

```text
                                   libp2p module   zk crate   zk prove / verify
opt-level 3                        7.05 MB         2.53 MB    632 ms / 6.4 ms
lto = "fat", codegen-units = 1     6.03 MB         2.51 MB    633 ms / 6.6 ms
lto = "fat", opt-level = "s"       4.06 MB         2.25 MB    677 ms / 7.6 ms
lto = "fat", opt-level = "z"       4.05 MB         2.12 MB    781 ms / 8.5 ms
```

This module is built with **`lto = "fat"`, `codegen-units = 1`, `opt-level = "s"`,
`panic = "unwind"`** and stripped: 42 % smaller, and 200 warm DHT lookups took the same time within
noise. Those numbers are the table's feature set. What the module carries today — relay, DCUtR,
AutoNAT, gossipsub, ping, request-response on top of it — is 11.6 MB of text in the shipped object,
and a C binary that links it is 13.6 MB stripped. Everything in that list is required by
`../../Architecture.md`; the profile is what keeps the number from being twice as large. `"z"` buys nothing more here. `lto = "thin"` made it larger. `panic = "abort"` would save
another megabyte and is excluded — a panic must come back as an error code, not end the server.
The zk crate keeps `opt-level = 3`, because there proving time is the product.

### Getting it

The build fetches the object like any other dependency: by URL and hash, per target triple. A
platform without a prebuild has no fast path until one is published, which is the cost named in
`../../Architecture.md`, *Setup*. Building from source stays possible for whoever has cargo, and a
locally built object passed to the build replaces the download: `-Dlibp2p-ffi=<dir>`, see *Today's
code*.

The module repository publishes them: a merged pull request whose title says *release* and whose
version has not been released before — not tagged, after every tag — builds on a native runner for
each of six targets — Linux, macOS and Windows, x64 and arm64 — and attaches one archive per target to a
GitHub release, with a `SHA256SUMS` over the archives. An archive holds the object, the header,
and `NATIVE_LIBS.txt`: the libraries the link line needs, printed by rustc rather than written
down, so this repository's build reads them instead of guessing them per platform. **Windows gets
the staticlib rather than a localized object** — MSVC's toolchain has no partial link — which
matters here only if a second Rust staticlib is ever linked into the same Windows binary. **On macOS
one symbol besides `lp2p_*` stays global: `_rust_eh_personality`, weak** — a Mach-O partial link
cannot hide it without breaking the references `compiler_builtins` makes to it. Beside the zk
crate that means the linker keeps one of the two personalities instead of reporting a duplicate.

---

## Memory

libp2p has no arenas and no pools. Everything goes through the Rust global allocator — async-style
code with a `Box` per future and stream, a `Vec` per protobuf message, `Arc` for addresses, and
per-frame buffers in Noise. 92 % of all allocations are 128 bytes or smaller. Measured with a
counting allocator over 50 nodes on loopback, both sides of every exchange counted:

```text
node start (TCP / QUIC)                ~300 / ~440 malloc, ~1 MB / ~3 MB resident
connection, transport only (TCP/QUIC)  ~230 / ~640 malloc, ~37 / ~70 KB per endpoint
connection with kad + identify         ~540 / ~920 malloc, ~45 / ~105 KB per endpoint
one kad request                        ~440 malloc, ~75 realloc, ~440 free, ~78 KB churn
one lookup (~30 requests at 50 nodes)  ~13,700 malloc
idle, open connections                 TCP: none in 20 s. QUIC: ~0.7 per connection per s
after shutdown                         back to baseline; no leak
```

None of that is visible to the caller and none of it can be arranged from outside. What the
boundary controls is the other half: nothing the module allocates crosses it. Output goes into
the caller's buffer as a copy, so the caller's arena discipline holds across the call. At the
scale Gradido targets malloc is not the cost — handshakes are, and connections that stay open are
cheap.

---

## The boundary

The shape matters more than the names, and it has to be right the first time: this is an interface
of a module that is meant to be released rarely, so it is built to **only grow**. New fields go at
the end of a struct, new event types and options get new numbers, and nothing is renamed,
renumbered or reused.

```c
/* libp2p_ffi.h -- every function is thread-safe and never blocks on the network, except
 * lp2p_poll with a timeout. A panic becomes LP2P_ERR_PANIC and poisons the handle. */

typedef uint8_t lp2p_key[32];        /* ed25519 public key */

typedef struct lp2p_options {
    uint32_t struct_size;            /* sizeof as the caller compiled it: a newer module
                                        reads what the caller knows, defaults the rest */
    uint8_t  node_seed[32];          /* this node's key; never the group key */
    const uint8_t *delegation;       /* this node, signed by the group key: LP2P_DELEGATION_BYTES */
    size_t   delegation_len;
    lp2p_key group;
    const char *const *listen_addrs; size_t listen_addr_count;   /* copied at start */
    const char *dht_protocol;
    const char *const *rpc_protocols; size_t rpc_protocol_count; /* index = protocol id */
    uint32_t rpc_max_request_bytes, rpc_max_response_bytes, rpc_timeout_ms;
    uint8_t  quic, dcutr, autonat;
    uint32_t max_connections, max_connections_per_peer, max_pending_incoming;
    lp2p_relay_options    relay;     /* server on by default where reachable; every limit */
    lp2p_announce_options announce;  /* on by default: topic, payload bound */
    size_t   event_queue_bytes;      /* overflow is reported, never silent */
    uint8_t  reachability;           /* public, private, or left to AutoNAT */
    uint32_t topic_max_message_bytes, topic_max_subscriptions;
} lp2p_options;

void    lp2p_options_default(lp2p_options *opt);
int32_t lp2p_start(const lp2p_options *opt, lp2p **out);
int32_t lp2p_shutdown(lp2p *node);

/* Events: whole, length-prefixed records into the caller's buffer, so an unknown type is
 * skipped. The dht-node role thread does nothing but wait here. */
int32_t lp2p_poll(lp2p *node, uint8_t *buf, size_t cap, int32_t timeout_ms);

/* RPC. The target is a group: the module looks up its nodes, checks their delegations and
 * fails over. `node_key` may pin one; zero means any. */
int32_t lp2p_rpc_request(lp2p *node, const lp2p_key group, const lp2p_key node_key,
                         uint16_t protocol, const uint8_t *data, size_t len,
                         uint32_t timeout_ms, uint64_t *request_id);
int32_t lp2p_rpc_respond(lp2p *node, uint64_t request_id, const uint8_t *data, size_t len);
int32_t lp2p_rpc_reject (lp2p *node, uint64_t request_id);

/* Policy from outside. Classes are numbers; what they mean is the caller's business. */
int32_t lp2p_peer_set_class(lp2p *node, const lp2p_key group, uint8_t peer_class);
int32_t lp2p_limit_set(lp2p *node, uint8_t peer_class, uint8_t scope, uint16_t protocol,
                       lp2p_rate rate);     /* scope: peer, ip prefix, global */
int32_t lp2p_limit_set_bytes(lp2p *node, uint8_t peer_class, uint8_t scope, uint16_t protocol,
                       lp2p_rate rate);     /* the same, counted in bytes on the wire */
int32_t lp2p_announce_set_payload(lp2p *node, const uint8_t *data, size_t len);

/* Topics: what several nodes read at once, without any of them asking. A topic key is 32
 * bytes the caller derives; the module keeps no meaning of its own in it. Subscribing also
 * provides the key in the DHT and looks it up there, so members that never meet otherwise
 * find each other -- gossipsub itself never dials to fill a mesh. */
int32_t lp2p_topic_subscribe  (lp2p *node, const lp2p_key topic_key);
int32_t lp2p_topic_unsubscribe(lp2p *node, const lp2p_key topic_key);
int32_t lp2p_topic_publish    (lp2p *node, const lp2p_key topic_key,
                               const uint8_t *data, size_t len);
int32_t lp2p_topic_peers      (const lp2p *node, const lp2p_key topic_key); /* 0: reaches nobody */

/* Network. */
int32_t lp2p_add_address(lp2p *node, const lp2p_key node_key, const char *multiaddr);
int32_t lp2p_dht_bootstrap(lp2p *node, uint64_t *query_id);
int32_t lp2p_dht_random_walk(lp2p *node, uint64_t *query_id);
/* Provider records under a key of the caller's choosing, the way a group is found. */
int32_t lp2p_dht_provide       (lp2p *node, const lp2p_key record_key);
int32_t lp2p_dht_stop_providing(lp2p *node, const lp2p_key record_key);
int32_t lp2p_dht_find_providers(lp2p *node, const lp2p_key record_key, uint64_t *query_id);
int32_t lp2p_routing_sample(lp2p *node, uint8_t *buf, size_t cap, uint32_t max_peers);
int32_t lp2p_stats_get(const lp2p *node, lp2p_stats *out);   /* out->size set by caller */
```

```text
events   LISTENING, REACHABILITY, PEER_CONNECTED, PEER_DISCONNECTED,
         RPC_REQUEST   group and node (delegation checked), protocol id, remote ip,
                       payload -- answered with respond or reject
         RPC_RESPONSE, RPC_FAILED (timeout / unreachable / refused / limited),
         ANNOUNCEMENT  group, node, payload -- whether it is new is the caller's call
         TOPIC_MESSAGE group, node (delegation checked), data: topic key, then payload
         PEER_DISCOVERED (random walk and find_providers), DHT_RESULT, LIMITED, OVERFLOW,
         HOLE_PUNCH
```

In Gradido's terms a group is a community and a node is one of its instances; the peer classes are
the five in `../../Architecture.md`, *With and without a URL*, and their numbers and limits are
contracted there, not here. A topic key is likewise nothing to the module: that a shard is
`sha256("gradido/shard/v1" || community key)[0] & 0x3f` and its topic
`sha256("gradido/shard/v1/topic" || shard)` is decided in `../../Architecture.md`, *Mirror nodes,
and the topics they follow*, and the module only ever sees the 32 bytes that come out.

**A message is limited where it is received, not where it is sent.** `lp2p_limit_set_bytes` with
`LP2P_PROTOCOL_TOPICS` gives a class a byte rate for published messages beside the message rate;
whichever runs out first stops the message, which is then neither reported to the caller nor
forwarded to anyone else. The class is the publisher's, the bucket is the peer that passed the
message on — that is the connection this node can actually stop. This is what sizes a mirror to
its hardware.

**Threading.** libp2p runs a tokio runtime; h2o runs event loops. Neither may block the other, so
the module owns its threads and the C side reaches them through calls and `lp2p_poll`. Nothing
calls back into C from a tokio thread. The dht-node role thread waits in the poll and hands events
to federation or backend — in the process when those roles run there, over their contracted
interfaces when they do not.

**Nothing crosses but bytes and lengths.** No Rust type, no string or buffer the caller has to
free, no pointer into the module after the call returns. Keys are 32-byte ed25519 public keys; a
peer with another key type is refused, because every node of a network built on this module runs
it or a mirror that follows the same contract.

**Panics stop at the boundary.** Every exported function catches, logs and returns an error code. A
panic unwinding into h2o's event loop is undefined behavior for the same reason a C++ exception is.

### What libp2p provides, and what the module adds

```text
libp2p                                   the module adds
connection-limits  counts, per peer      a token bucket per class: per peer, per IP
memory-connection-limits                 prefix, global -- checked once a request and its
request-response   concurrent streams,   delegation have arrived, before the caller sees
                   timeout, size limit   it. libp2p has no per-peer request rate.
relay              reservations, circuits, nothing: exposed as options. There is no
                   duration, bytes per       bytes-per-second cap; the bound is what the
                   circuit, rate per peer    limits multiply to.
                   and per IP
kad                lookup, providers,     failover across a group's nodes,
                   records                delegation check on connect
gossipsub          signed messages,        the announcement payload, set from outside;
                   mesh among connected   topics with a mesh bootstrapped over the DHT --
                   peers                  provide on subscribe, look up, dial a few, and
                                          repeat every 30 s while a topic has no peer.
                                          Per-class byte and message limits on what
                                          arrives. libp2p never dials for a mesh.
```

Relay defaults as libp2p ships them, and as the module starts with: 128 reservations, 4 per peer,
1 h each; 16 concurrent circuits, 4 per peer, 2 min and 128 KiB each; one new circuit per 2 min per
peer (30 in reserve) and per minute per IP (60 in reserve).

---

## Safety

Safe Rust ends at the `extern "C"` line, so the module is not exempt from anything in
`../Architecture.md`, *Safety net*:

- `#![forbid(unsafe_code)]` in the interior. The `unsafe` lives in one file — the one that turns
  caller pointers into slices — small enough to review in one sitting, and fuzzed like a parser in
  the module repository, because what reaches it was said by strangers on the network.
- The sanitizers run over the linked binary here. The prebuild is not instrumented, so they see
  the C side of the seam: the pointers, lengths and buffers this repository passes in.

---

## Versions and interop

`rust-libp2p` is pinned inside the module and `js-libp2p` in `packages/dht-node`. Two libraries with
their own release cycles can diverge on a protocol detail without either being wrong, and a module
that is rarely released stays in the network for years. So the interop test has two pairings, and
both are the merge gate — in the module repository for a new release, and here for raising the
pinned version:

```text
js-libp2p node       <->  prebuild           discover, call, relay, announce
previous prebuild    <->  new prebuild       the same, because the network runs both
```

The first pairing exists: `libp2p-ffi/interop/js` runs js-libp2p under Bun against the module --
calls, announcements, DHT lookups and relaying in both directions, and QUIC, which interoperates.
It found two things the module had to change: the provider key became a sha2-256 multihash, because
js-libp2p's DHT names keys by CID, and the module runs ping, because a js DHT keeps no peer it cannot
ping. `packages/dht-node/Architecture.md`, *What was verified*, has what the TypeScript side needs.
The second pairing, old prebuild against new, starts with the first release.

---

## Today's code

**The binary links libp2p-ffi v0.1.2.** `build.zig.zon` pins the six release archives by hash, as
lazy dependencies, and `build.zig` fetches the one the target needs; `CMakeLists.txt` pins the same
archives by the SHA-256 the release publishes. Both read the archive's `NATIVE_LIBS.txt` rather than
keeping a list of their own. `zig build` puts zig's libunwind where the list names `-lgcc_s`, because
a cross build has no libgcc_s of the target to find, and it stops on any entry it does not know. The
CMake build uses the host's toolchain and passes the list on as it is. `include/libp2p_ffi.h` is a
byte-for-byte copy of the release's header.

```text
-Dlibp2p-ffi=prebuilt   the default: the pinned archive where one is published for the target
            =stub       src/libp2p_ffi_stub.c, which reaches nobody
            =<dir>      a local build, as libp2p-ffi's scripts/localize.sh leaves it in dist/<triple>
                        (CMake: -DFS_LIBP2P_FFI=prebuilt|stub|<dir>)
```

**The stub stands in where no prebuild is published**: musl and windows-gnu, which are what
`zig build` cross compiles to by default. Its `lp2p_start` warns that the node reaches nobody, and a
call answers `LP2P_ERR_UNAVAILABLE` rather than pretending to have been sent. A windows-gnu binary
does not take the MSVC archive, because the Rust object expects MSVC's CRT and exception handling;
CMake is the Windows build that links it.

`dht-node/tests/test_libp2p_ffi.cpp` starts two nodes on loopback inside this build and makes one
call the other. It proves what the module's own tests cannot, that the object links and runs with
this toolchain: its threads, the unwinder zig substitutes, a real round trip. It is built only where
a module is linked. Verified natively, and as cross builds for aarch64-linux-gnu (the ARM archive)
and for musl and windows-gnu (the stub). macOS and the MSVC build need their own hosts and have not
been built here.

`src/dht_node_server.c` is the role as it will stay — options from the configuration, a thread
that waits in the poll, events handed on — except that an inbound call is rejected until there is a
federation role to hand it to. **Its identity comes from `setup`.** The node seed is derived from
`MASTER_SEED` along `dht` (`service_core/master_seed.h`), read where it is used and wiped after
`lp2p_start`. The delegation and with it the community key are `DHT_DELEGATION`, which
`setup` has the community key sign. The role checks that the delegation names the node the seed
derives before it starts anything, because a seed replaced after `setup` signed is the one way the
two drift apart. `dht.node.failed` names what is wrong in its `reason` — `master-seed-missing`,
`master-seed-invalid`, `delegation-missing`, `delegation-invalid`, `delegation-foreign` — and each
message says to run `setup`.

**It joins through `DHT_BOOTSTRAP_URL`** (`src/bootstrap.c`), `https://gdd.gradido.net` when unset
and nobody when empty: `peer.bootstrap` fetched through curl, `self.delegation` checked to name
`self.peerId`, every node of the answer handed to `lp2p_add_address`, then `lp2p_dht_bootstrap`. At
start, and again every minute while the node has no connection. The fetch blocks the role's thread
for ten seconds at most and gives up at once on quit; the module queues events meanwhile. Peer ids
in text are `src/peer_id.c`, base58 as js-libp2p writes them.

**It answers `peer.bootstrap` for the backend beside it.** The role registers an answer in
`service_core/peer_network.h` — itself from its `LP2P_EV_LISTENING` addresses, peers from
`lp2p_routing_sample` — and `backend/src/peer_routes.c` serves it; without the role in the process
the route answers `PEER_NETWORK_UNAVAILABLE`. The registration is cleared before the node shuts
down, under the lock an answer in progress holds.

**The role applies the contracted request limits at start** — `DHT_LIMIT_*` per `DHT_CLASS_*` and
scope, through `lp2p_limit_set`; `packages/dht-node/src/limits.ts` applies the same table on the
other path. Until something hands classes over, every community is `DHT_CLASS_UNKNOWN`, so the
strictest row is the one in force.

The module exists (`../libp2p-ffi`) and covers the design above: Kademlia with providers under the
group key, RPC with failover and the delegation checked on every frame, circuit relay for PRIVATE
nodes, peer classes with token-bucket limits in messages and in bytes, signed announcements over
gossipsub, and topics whose mesh is bootstrapped over the DHT. It is tested on loopback and linked
from C through the localized object; the topic tests cover a message that reaches a node the
publisher cannot dial at all, and two members that find each other through nothing but the
provider record under the topic key. AutoNAT decides reachability for a node
configured UNKNOWN; a configured PUBLIC or PRIVATE is never overridden. Hole punching is verified
through NAT in Docker (`interop/holepunch` there): TCP and QUIC connections are upgraded from relayed
to direct through cone NAT and stay relayed, still working, through symmetric NAT. Its README keeps
the status.

The role in `src/dht_node_server.c` follows no topic yet. What a node mirrors is contracted as four
settings in `contracts/settings.json` — `dht.mirror_shards` (default `own`),
`dht.mirror_communities`, `dht.mirror_bytes_per_second` (64 KiB/s) and
`dht.mirror_bytes_burst` (1 MiB) — and waits for the settings table, and for the role that
reads it to hand the values over: this one reads no database. Until then the defaults are what a
node would apply.

**Reachability is configured, never discovered.** `DHT_REACHABILITY` is `public` or
`private`, and private when unset. `setup` writes it from the community URL
(`service_core/public_url.h`): public for a domain or an address on the public internet, private for
localhost, loopback, the private and link-local ranges, CGNAT, names without a dot and the reserved
suffixes. `contracts/test-vectors/public-url.json` holds both paths to the same answers. A PUBLIC node
announces its own addresses and relays for others from its first second; a PRIVATE one reserves on
relays and announces only relayed addresses. A configured value is never overridden by AutoNAT. An
operator whose URL is public but whose dht port is not forwarded writes `private` by hand.

`DHT_TOPIC` — legacy's `FEDERATION_DHT_TOPIC` — separates networks and is part of every protocol
name the role builds, `DHT_KAD_PROTOCOL` and `DHT_ANNOUNCE_TOPIC` in `contracts/const.json`.

`packages/dht-node/interop/fast.test.ts` holds the role to the reference node: this binary joins
through a peer.bootstrap a TypeScript node answers, a TypeScript node joins through the one a
set-up instance of this binary serves, and each finds the other's community by its key. `bun run
test:interop` at the root builds the binary and runs it.

---

## Open

Shared with the TypeScript node and recorded in `../../Architecture.md`, *Peer network*, *Open*:
the envelope on the wire, a byte limit across classes, the signed bootstrap answer, data
availability. Answering any of them for one node only is the failure mode.
