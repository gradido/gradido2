# packages/dht-node Architecture

The network node on the reference path: `js-libp2p`.

**Read `../../Architecture.md`, *Peer network*, first.** It holds what the node is for, why it is
mirrored rather than shared, how communities and their instances are addressed, the RPC rules and
the discovery design. This file holds only what is specific to the TypeScript half.

Its counterpart is [`fast-servers/dht-node`](../../fast-servers/dht-node/Architecture.md):
`rust-libp2p` through [`libp2p-ffi`](https://github.com/gradido/libp2p-ffi), prebuilt, behind an
`extern "C"` header. The two are kept together by the libp2p
specification, by what is contracted on top of it, and by an interop test — not by anyone
remembering to change both.

---

## What this package is not

```text
not a database client    it reports what it learns; the federation role persists it
not a domain component   it knows no RPC operation and verifies no envelope
not a scheduler          random walks and bootstrap are calls from outside
not a list of communities a community is looked up by key when it is called
```

Legacy's `dht-node` writes `federated_communities` rows itself. That is the one behavior deliberately
not carried over — see the fast-path file for why; the reasoning is the same on both sides.

---

## What must match the other half

More than peer discovery ever needed, which is exactly why this list exists:

```text
contract vectors   master-seed.json   MASTER_SEED -> node seed and key, and the delegation the
                                      community key signs for it
                   shard.json         a community's shard, its shard and community topic keys
                   public-url.json    which community URL makes the node PUBLIC
                   still missing: the RPC envelope -- bytes, signature, nonce
contracts          const.json         DHT_KAD_PROTOCOL, DHT_ANNOUNCE_TOPIC, DHT_RPC_PROTOCOL,
                                      DHT_ANNOUNCE_MAX_PAYLOAD_BYTES, DHT_CLASS_*, DHT_LIMIT_*
                                      (requests per class and scope), DHT_RPC_MAX_*_BYTES,
                                      DHT_RELAY_*, DHT_BOOTSTRAP_*
                   server/backend/peer.json   peer.bootstrap: what a node answers and a
                                      joining node checks
                   logging.json       the dht events, the same on both nodes
                   libp2p-ffi         the framing -- request written and write side closed, the
                                      response read to the end -- the provider key
                                      CIDv1(raw, sha2-256(community key)), the topic name
                                      "/lp2p/topic/1/" + key in lowercase hex
                   still missing: the RPC operation names
interop test       interop/fast.test.ts against gradido2-fast: bootstrap both ways, discovery by
                   community key, a call dropped at once. Still to come: fail over, relay,
                   announce
```

The framing is ours on both sides on purpose. Nothing then depends on either library's
request-response helper — the Rust one's codecs are defined only by Rust — and the bytes on a
stream are something a contract can describe.

---

## Shape

It is the same node as the fast path's, expressed in TypeScript rather than behind a C header:
the same variables, the same checks in the same order, the same log events. Where the C side waits
in `lp2p_poll` for what libp2p-ffi reports, this side listens to js-libp2p's events and does itself
what the module does there: check the delegation on every call and announcement, apply the limits
per class, publish the provider record under the community key. Everything the fast-path boundary
says about what the node decides — nothing — holds here too.

```text
src/main.ts       runDhtNode: DHT_TOPIC, the identity, the node, graceful shutdown
src/config/       DHT_TOPIC, DHT_PORT, MASTER_SEED, DHT_DELEGATION, DHT_REACHABILITY,
                  DHT_BOOTSTRAP_URL
src/bootstrap.ts  joining through another community's peer.bootstrap
src/identity.ts   node key from MASTER_SEED along `dht`, the delegation checked against it
src/node.ts       js-libp2p: TCP, QUIC, relay, noise, yamux, identify, ping, kad-dht,
                  gossipsub, autonat, dcutr; the RPC handler, the announcement validator
src/wire.ts       frames, delegation, provider key -- libp2p-ffi's src/wire.rs and delegation.rs
src/limits.ts     the token buckets per class and scope, and the announcement rate
```

The node runs as the dht-node role of the single binary, alone or beside backend and federation. In
the same process it hands an inbound call to federation directly; on its own it forwards over HTTP.
**It runs in one process per instance key**: a deployment that scales the backend over
`SO_REUSEPORT` processes starts the node in one of them or beside them, never in each — two
processes with one key are one peer id in two places, and libp2p spreads calls across them
instead of failing over.

---

## Today's code

The node does what the C role does and no more: it joins under the identity `setup` wrote, provides
under its community key, reports announcements, drops a call naming an operation it does not take
and refuses the rest, because handing a call to a federation role is built on neither path yet — and
no operation is taken yet either. `gradido2 dht-node` starts it from the single binary, and
`gradido2 backend dht-node` starts it beside the backend, which then answers `peer.bootstrap` from it
through `registerPeerNetwork` in `@gradido/service-core`. `scripts/bundle.ts` builds with
`scripts/quicBinding.ts`, without which the QUIC binding would be left out of the executable.

It joins through `DHT_BOOTSTRAP_URL` (`src/bootstrap.ts`): the answer's `self.delegation` checked to
name `self.peerId`, every node of it dialled, at start and again every minute while the node has no
connection. Its own answer is itself and a rotating slice of the peers it is connected to that speak
its DHT protocol.

`interop/fast.test.ts` holds it to the C role of this repository: the C node joins through an answer
this node gives, this node joins through the answer a set-up C instance serves beside its node, and
each finds the other's community by its key alone. Both derive the same node key from one master
seed, so the same peer id. `src/node.test.ts` and `src/bootstrap.test.ts` hold the TypeScript side of
this on loopback.

**js-libp2p's `stop()` does not return when the peer at the other end closes the same connection
at the same moment** — two servers restarted together, or two nodes in one test. Stopped one after
the other, each takes about 12 ms. The node therefore waits at most five seconds, and
`dht.node.stopped` says with `complete: false` that it left the rest to the process exit.

## What was verified, and what it requires

`libp2p-ffi/interop/js` runs js-libp2p 3.3 under Bun 1.3 against the Rust module, on loopback: RPC,
announcements and DHT lookups in both directions, QUIC, and relaying in both directions -- a js
node without a reachable address behind a Rust relay, and a private Rust node behind a js relay.
A node with every service, QUIC's native binding included, also runs as a `bun build --compile`
binary started away from its `node_modules`. All of it works. None of it works without these, and
each one fails silently rather than loudly:

- **The provider key is a CID.** js-libp2p's DHT names keys by CID only, so a community's instances
  provide under `CIDv1(raw, sha2-256(community key))`. The Rust module was changed to the same key.
- **Run `@libp2p/ping`.** kad-dht requires it and pings every new peer before it keeps it in its
  routing table; the Rust module was given ping for the same reason.
- **`runOnLimitedConnection: true` on the RPC handler and on every RPC dial.** js-libp2p refuses
  streams on relayed connections otherwise, and a community without a URL is reached over nothing
  else.
- **Build with the QUIC binding plugin.** `@chainsafe/libp2p-quic` picks its native binding at run
  time where the bundler cannot see it; `scripts/bundle.ts` needs the plugin from
  `libp2p-ffi/interop/js/quic-binding-plugin.ts`, or the binary starts without QUIC.
- **The wire is `interop/js/lp2p.ts`.** Delegation, frames, provider key and topic name, byte for
  byte what the Rust module does; `packages/dht-node` starts from that file.

Topics are verified the same way: a Rust node publishes into a topic and the js node receives it,
and back, with the delegation checked on both sides. Two things this side has to do itself, because
the Rust module does them inside `lp2p_topic_subscribe`:

- **Bootstrap the mesh over the DHT.** gossipsub forwards between peers that are already connected
  and never dials to find more, so a topic with a handful of members stays silent unless somebody
  dials. On subscribe, provide the topic key (`CIDv1(raw, sha2-256(topic key))`, the same shape as
  the provider key), find its providers, dial a few, and repeat while the topic has no peer. Without
  it a mirror that follows a small shard receives nothing and reports no error.
- **Limit what arrives, in bytes.** js-libp2p has no per-peer byte rate for pubsub any more than it
  has one for requests; it is written here, against the same classes and numbers, and a message over
  the limit is neither reported nor forwarded.

Through NAT, in `libp2p-ffi/interop/holepunch`, two limits of js-libp2p showed that no configuration
removes:

- **A TypeScript node behind NAT does not hole punch.** js-libp2p's DCUtR offers only addresses its
  AutoNAT verified, and behind NAT none is ever verified. Forcing the observed addresses through gets
  exactly one case direct -- a js dialer to a Rust listener over QUIC -- because js-libp2p's QUIC dials
  from a fresh socket each time and its TCP cannot reuse the listen port. Calls still work: every
  pairing gets its answer over the relay.
- **js-libp2p's AutoNAT cannot decide reachability.** Its client has no private verdict (4 successes or
  8 failures from different /8 networks), and its server closes the connection a request arrives on
  -- no client, Rust or js, gets an answer from it. A Rust server answers js clients correctly.

So the TypeScript node configures reachability rather than discovering it: a community with a URL is
PUBLIC, one without is PRIVATE and lives on relays. That is a real difference to the fast path, where
a community without a URL is upgraded to a direct connection through cone NAT. Whether it counts as
"too much" under *The way out* is a decision to take with these numbers, not before them. **Rate limiting** is not in js-libp2p as a
per-peer request rate either; it is written here, against the same classes and numbers as the Rust
node.

---

## The way out

Mirroring is the plan, and it has a stated exit. If keeping this node in step with the prebuild costs
too much, the TypeScript path loads the same prebuilt module through an addon — linked the way
`shared-native` links `gradido-blockchain-core`, from the same symbol-localized `libp2p_ffi.o` the
fast path links — and the node becomes single-implementation.

What counts as "too much" is decided on evidence, and the evidence is concrete:

- a `js-libp2p` upgrade that breaks the interop test and has to be worked around rather than pinned
  past
- a feature of the network that has to be written twice rather than configured twice
- a Bun incompatibility that needs a patch in a dependency

The switch is recorded in `../../Architecture.md` before it is made. It changes where the TypeScript
path's continuity comes from — a downloaded prebuild instead of an npm dependency — and that is a
decision, not a refactoring.

---

## Open

All of it is shared with the fast path and recorded in `../../Architecture.md`, *Peer network*,
*Open*. Answering any of it for one node only is the failure mode.
