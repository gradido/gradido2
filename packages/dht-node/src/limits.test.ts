import { describe, expect, test } from 'bun:test'
import { AnnouncementRate, DHT_CLASS, Limits } from './limits'

const group = new Uint8Array(32).fill(1)
const caller = (peer: string, ip: string | undefined = undefined) => ({ group, peer, ip })

describe('Limits', () => {
  test('an unknown community gets its burst per node, then one every ten seconds', () => {
    const limits = new Limits()
    const now = 1_000_000
    for (let i = 0; i < 3; ++i) {
      expect(limits.admit(caller('a'), now)).toBeUndefined()
    }
    expect(limits.admit(caller('a'), now)).toBe('peer')
    // Another node has a bucket of its own.
    expect(limits.admit(caller('b'), now)).toBeUndefined()
    // DHT_LIMIT_UNKNOWN_PEER_PER_MINUTE is 6: one token every ten seconds.
    expect(limits.admit(caller('a'), now + 10_000)).toBeUndefined()
    expect(limits.admit(caller('a'), now + 10_000)).toBe('peer')
  })

  test('a prefix is shared, and a relayed connection has none', () => {
    const limits = new Limits()
    const now = 1_000_000
    // DHT_LIMIT_UNKNOWN_PREFIX_BURST is 10; every call comes from another node of one /24.
    for (let i = 0; i < 10; ++i) {
      expect(limits.admit(caller(`n${i}`, '10.0.0.1'), now)).toBeUndefined()
    }
    expect(limits.admit(caller('n10', '10.0.0.200'), now)).toBe('prefix')
    expect(limits.admit(caller('n11', '10.0.1.1'), now)).toBeUndefined()
    expect(limits.admit(caller('n12'), now)).toBeUndefined()
  })

  test('an IPv6 prefix is the first 56 bits, however the address is written', () => {
    const limits = new Limits()
    const now = 1_000_000
    // Three groups and the high byte of the fourth: ab00 to ab09 are one /56.
    for (let i = 0; i < 9; ++i) {
      expect(limits.admit(caller(`n${i}`, `2001:db8:1:ab0${i}::1`), now)).toBeUndefined()
    }
    expect(limits.admit(caller('n9', '2001:0db8:0001:ab09:0:0:0:1'), now)).toBeUndefined()
    expect(limits.admit(caller('n10', '2001:db8:1:abff::7'), now)).toBe('prefix')
    expect(limits.admit(caller('n11', '2001:db8:1:ac00::1'), now)).toBeUndefined()
  })

  test('a refused call costs the other limits nothing', () => {
    const limits = new Limits()
    const now = 1_000_000
    // Use up the global bucket of the class with callers from many prefixes.
    for (let i = 0; i < 60; ++i) {
      expect(limits.admit(caller(`g${i}`, `10.${i}.0.1`), now)).toBeUndefined()
    }
    expect(limits.admit(caller('late', '10.200.0.1'), now)).toBe('global')
    expect(limits.admit(caller('late', '10.200.0.1'), now + 1000)).toBeUndefined()
  })

  test('classes: blocked is refused, own instances are not limited, unknown is the default', () => {
    const limits = new Limits()
    expect(limits.classOf(group)).toBe(DHT_CLASS.unknown)
    limits.setClass(group, DHT_CLASS.blocked)
    expect(limits.admit(caller('a'))).toBe('blocked')
    limits.setClass(group, DHT_CLASS.ownInstance)
    for (let i = 0; i < 100; ++i) {
      expect(limits.admit(caller('a'))).toBeUndefined()
    }
    limits.setClass(group, DHT_CLASS.unknown)
    expect(limits.classOf(group)).toBe(DHT_CLASS.unknown)
  })
})

describe('AnnouncementRate', () => {
  test('three in reserve per source, then one every ten seconds', () => {
    const rate = new AnnouncementRate()
    expect([1, 2, 3].map(() => rate.allow('a', 0))).toEqual([true, true, true])
    expect(rate.allow('a', 0)).toBe(false)
    expect(rate.allow('b', 0)).toBe(true)
    expect(rate.allow('a', 10_000)).toBe(true)
  })
})
