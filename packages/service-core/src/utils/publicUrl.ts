/**
 * Whether a URL names a host on the public internet: a domain, or an address that is routed there.
 *
 * `setup` decides with it whether the dht node starts PUBLIC or PRIVATE. The C half is
 * `fast-servers/service-core/src/public_url.c`, and `contracts/test-vectors/public-url.json` holds
 * both to the same answers. So the host is taken out of the text by hand rather than by the WHATWG
 * parser, which rewrites `http://2130706433` into `127.0.0.1` where no C reader would: a host is
 * judged as it was written.
 *
 * ```text
 * private   no scheme, an empty host, a zone id (%)
 *           a name without a dot, or under localhost, local, internal, test, example,
 *           invalid, home.arpa
 *           an address in a form nobody publishes: a last label that is a number
 *           IPv4 0/8, 10/8, 100.64/10, 127/8, 169.254/16, 172.16/12, 192.168/16, 224/3
 *           IPv6 ::, ::1, fc00::/7, fe80::/10, ff00::/8, and ::ffff:0:0/96 by its IPv4
 * public    everything else
 * ```
 */
export function isPublicUrl(url: string): boolean {
  const host = hostOf(url)
  if (host === undefined || host.includes('%')) {
    return false
  }
  if (host.startsWith('[')) {
    const address = parseIpv6(host.slice(1, -1))
    return address !== undefined && isPublicIpv6(address)
  }
  const ipv4 = parseIpv4(host)
  if (ipv4 !== undefined) {
    return isPublicIpv4(ipv4)
  }
  return isPublicName(host)
}

/** The host as written, ASCII lowercased, an IPv6 literal with its brackets; or undefined. */
function hostOf(url: string): string | undefined {
  const scheme = url.indexOf('://')
  if (scheme < 0) {
    return undefined
  }
  const rest = url.slice(scheme + 3)
  let authority = rest.slice(0, firstOf(rest, '/?#'))
  authority = authority.slice(authority.lastIndexOf('@') + 1)
  let host: string
  if (authority.startsWith('[')) {
    const close = authority.indexOf(']')
    if (close < 0) {
      return undefined
    }
    host = authority.slice(0, close + 1)
  } else {
    host = authority.slice(0, firstOf(authority, ':'))
  }
  host = host.replace(/[A-Z]/gu, (c) => c.toLowerCase())
  return host === '' || host === '[]' ? undefined : host
}

function firstOf(text: string, stops: string): number {
  for (let i = 0; i < text.length; ++i) {
    if (stops.includes(text[i] as string)) {
      return i
    }
  }
  return text.length
}

const PRIVATE_TOP_LEVEL = new Set(['localhost', 'local', 'internal', 'test', 'example', 'invalid'])

function isPublicName(host: string): boolean {
  const labels = host.split('.')
  if (labels.at(-1) === '') {
    labels.pop()
  }
  if (labels.length < 2 || labels.includes('')) {
    return false
  }
  const last = labels.at(-1) as string
  if (/^[0-9]+$/u.test(last) || /^0x[0-9a-f]*$/u.test(last)) {
    return false
  }
  if (PRIVATE_TOP_LEVEL.has(last)) {
    return false
  }
  return !(last === 'arpa' && labels.at(-2) === 'home')
}

/** Four decimal parts of at most 255, without leading zeros: what inet_pton takes. */
function parseIpv4(text: string): number[] | undefined {
  const parts = text.split('.')
  if (parts.length !== 4) {
    return undefined
  }
  const bytes: number[] = []
  for (const part of parts) {
    if (!/^(0|[1-9][0-9]{0,2})$/u.test(part) || Number(part) > 255) {
      return undefined
    }
    bytes.push(Number(part))
  }
  return bytes
}

/** RFC 4291 text as inet_pton takes it -- hex groups, one `::`, an IPv4 tail -- as 16 bytes. */
function parseIpv6(text: string): number[] | undefined {
  const halves = text.split('::')
  if (halves.length > 2) {
    return undefined
  }
  const compressed = halves.length === 2
  const head = words(halves[0] as string, !compressed)
  const tail = compressed ? words(halves[1] as string, true) : []
  if (head === undefined || tail === undefined) {
    return undefined
  }
  const missing = 8 - head.length - tail.length
  if (compressed ? missing < 1 : missing !== 0) {
    return undefined
  }
  const all = [...head, ...new Array<number>(compressed ? missing : 0).fill(0), ...tail]
  return all.flatMap((word) => [word >> 8, word & 0xff])
}

function words(text: string, ipv4Tail: boolean): number[] | undefined {
  if (text === '') {
    return []
  }
  const groups = text.split(':')
  const out: number[] = []
  for (let i = 0; i < groups.length; ++i) {
    const group = groups[i] as string
    if (ipv4Tail && i === groups.length - 1 && group.includes('.')) {
      const ipv4 = parseIpv4(group)
      if (ipv4 === undefined) {
        return undefined
      }
      out.push(
        ((ipv4[0] as number) << 8) | (ipv4[1] as number),
        ((ipv4[2] as number) << 8) | (ipv4[3] as number),
      )
    } else if (/^[0-9a-f]{1,4}$/u.test(group)) {
      out.push(Number.parseInt(group, 16))
    } else {
      return undefined
    }
  }
  return out
}

function isPublicIpv4([a, b]: number[]): boolean {
  const first = a as number
  const second = b as number
  return !(
    first === 0 ||
    first === 10 ||
    first === 127 ||
    first >= 224 ||
    (first === 100 && second >= 64 && second <= 127) ||
    (first === 169 && second === 254) ||
    (first === 172 && second >= 16 && second <= 31) ||
    (first === 192 && second === 168)
  )
}

function isPublicIpv6(bytes: number[]): boolean {
  const [first, second] = bytes as [number, number]
  const zeroTo = (end: number) => bytes.slice(0, end).every((byte) => byte === 0)
  if (zeroTo(15) && ((bytes[15] as number) === 0 || (bytes[15] as number) === 1)) {
    return false
  }
  if (zeroTo(10) && bytes[10] === 0xff && bytes[11] === 0xff) {
    return isPublicIpv4(bytes.slice(12))
  }
  return !(
    (first & 0xfe) === 0xfc ||
    (first === 0xfe && (second & 0xc0) === 0x80) ||
    first === 0xff
  )
}
