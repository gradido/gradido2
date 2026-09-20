/**
 * Peer classes and the inbound RPC limits per class -- DHT_CLASS_* and DHT_LIMIT_* in
 * contracts/const.json. What libp2p-ffi does in its src/limits.rs, which js-libp2p does not do at
 * all: a token bucket per class, scope and key, and a request goes through only if every bucket
 * that applies has a token left.
 *
 * ```text
 * peer     one bucket per node
 * prefix   one per IPv4 /24 or IPv6 /56 -- not for a relayed connection, whose address is the relay's
 * global   one for the whole class
 * ```
 *
 * A class belongs to a community. Which one it is is decided outside the node; until something
 * hands classes over, every community is DHT_CLASS_UNKNOWN.
 */

export const DHT_CLASS = {
  unknown: 0,
  withoutUrl: 1,
  withUrl: 2,
  ownInstance: 3,
  blocked: 255,
} as const

export type Scope = 'peer' | 'prefix' | 'global'

interface Rate {
  readonly perMinute: number
  readonly burst: number
}

/** DHT_LIMIT_<CLASS>_<SCOPE>_PER_MINUTE and _BURST. A class or scope that is absent is not limited. */
const LIMITS: ReadonlyMap<number, Readonly<Partial<Record<Scope, Rate>>>> = new Map([
  [
    DHT_CLASS.unknown,
    {
      peer: { perMinute: 6, burst: 3 },
      prefix: { perMinute: 30, burst: 10 },
      global: { perMinute: 300, burst: 60 },
    },
  ],
  [
    DHT_CLASS.withoutUrl,
    {
      peer: { perMinute: 30, burst: 10 },
      prefix: { perMinute: 120, burst: 30 },
      global: { perMinute: 1200, burst: 200 },
    },
  ],
  [
    DHT_CLASS.withUrl,
    { peer: { perMinute: 120, burst: 30 }, global: { perMinute: 6000, burst: 1000 } },
  ],
])

/** Buckets that have refilled completely are forgotten past this many. */
const MAX_BUCKETS = 1 << 16

interface Bucket {
  tokens: number
  updated: number
}

export interface Caller {
  /** The community key the caller's delegation names. */
  readonly group: Uint8Array
  /** The caller's peer id, as text. */
  readonly peer: string
  /** The address the connection comes from, or undefined for a relayed one. */
  readonly ip: string | undefined
}

export class Limits {
  private readonly classes = new Map<string, number>()
  private readonly buckets = new Map<string, Bucket>()

  public setClass(group: Uint8Array, peerClass: number): void {
    const key = Buffer.from(group).toString('hex')
    if (peerClass === DHT_CLASS.unknown) {
      this.classes.delete(key)
    } else {
      this.classes.set(key, peerClass)
    }
  }

  public classOf(group: Uint8Array): number {
    return this.classes.get(Buffer.from(group).toString('hex')) ?? DHT_CLASS.unknown
  }

  /** Takes a token for @p caller, or answers why not: `blocked`, or the scope that had none. */
  public admit(caller: Caller, now = Date.now()): Scope | 'blocked' | undefined {
    const peerClass = this.classOf(caller.group)
    if (peerClass === DHT_CLASS.blocked) {
      return 'blocked'
    }
    const rates = LIMITS.get(peerClass) ?? {}
    const matching: [string, Rate, Scope][] = []
    for (const scope of ['peer', 'prefix', 'global'] as const) {
      const rate = rates[scope]
      if (rate === undefined) {
        continue
      }
      const key = scope === 'peer' ? caller.peer : scope === 'prefix' ? prefixOf(caller.ip) : ''
      if (key !== undefined) {
        matching.push([`${peerClass}|${scope}|${key}`, rate, scope])
      }
    }
    /* Checked before anything is taken, so a request refused by one limit costs the others
       nothing. */
    for (const [key, rate, scope] of matching) {
      if (this.refilled(key, rate, now).tokens < 1) {
        return scope
      }
    }
    for (const [key, rate] of matching) {
      this.refilled(key, rate, now).tokens -= 1
    }
    if (this.buckets.size > MAX_BUCKETS) {
      this.buckets.clear()
    }
    return undefined
  }

  private refilled(key: string, rate: Rate, now: number): Bucket {
    let bucket = this.buckets.get(key)
    if (bucket === undefined) {
      bucket = { tokens: rate.burst, updated: now }
      this.buckets.set(key, bucket)
    }
    const perMs = rate.perMinute / 60_000
    bucket.tokens = Math.min(rate.burst, bucket.tokens + (now - bucket.updated) * perMs)
    bucket.updated = now
    return bucket
  }
}

/** IPv4 /24 or IPv6 /56 as text, or undefined without an address. */
function prefixOf(ip: string | undefined): string | undefined {
  if (ip === undefined) {
    return undefined
  }
  if (ip.includes('.')) {
    return `4:${ip.split('.').slice(0, 3).join('.')}`
  }
  /* The first seven bytes: three full groups and the high byte of the fourth, after expanding ::. */
  const [head = '', tail = ''] = ip.split('::')
  const heads = head === '' ? [] : head.split(':')
  const tails = tail === '' ? [] : tail.split(':')
  const groups = [
    ...heads,
    ...new Array<string>(8 - heads.length - tails.length).fill('0'),
    ...tails,
  ]
  const padded = groups.map((group) => group.padStart(4, '0'))
  return `6:${padded.slice(0, 3).join(':')}:${padded[3]?.slice(0, 2)}`
}

/**
 * Announcements per source: one every ten seconds, three in reserve -- libp2p-ffi's
 * ANNOUNCE_INTERVAL and ANNOUNCE_BURST. Gossipsub hands on whatever a peer publishes, and without
 * this one node could make every other node report a stream of them.
 */
export class AnnouncementRate {
  private readonly buckets = new Map<string, Bucket>()

  public allow(source: string, now = Date.now()): boolean {
    let bucket = this.buckets.get(source)
    if (bucket === undefined) {
      if (this.buckets.size > MAX_BUCKETS) {
        this.buckets.clear()
      }
      bucket = { tokens: 3, updated: now }
      this.buckets.set(source, bucket)
    }
    bucket.tokens = Math.min(3, bucket.tokens + (now - bucket.updated) / 10_000)
    bucket.updated = now
    if (bucket.tokens < 1) {
      return false
    }
    bucket.tokens -= 1
    return true
  }
}
