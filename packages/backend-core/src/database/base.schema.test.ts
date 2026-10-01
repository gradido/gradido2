import { describe, expect, test } from 'bun:test'
import { MAX, NIL, v1, v3, v4, v5, v6, v7, validate, version } from 'uuid'
import * as v from 'valibot'
import {
  buffer32Schema,
  dateSchema,
  uint8Array32Schema,
  uint8Array64Schema,
  uuidv4Schema,
} from './base.schema'

const hex = '39568d7e148a0afee7f27a67dbf7d4e87d1fdec958e2680df98a469690ffc1a2'
const buffer = Buffer.from(hex, 'hex')
const bytes = new Uint8Array(buffer)

describe('uint8Array32Schema', () => {
  test.each([
    ['hex', hex],
    ['a Uint8Array', bytes],
    ['a Buffer', buffer],
  ])('reads %s as a plain Uint8Array', (_, input) => {
    const output = v.parse(uint8Array32Schema, input)

    expect(output).toEqual(bytes)
    expect(Buffer.isBuffer(output)).toBe(false)
  })

  test('hands out a copy, not the bytes it was given', () => {
    expect(v.parse(uint8Array32Schema, bytes)).not.toBe(bytes)
  })

  test.each([
    ['one byte short', bytes.subarray(1)],
    ['one hex character short', hex.slice(1)],
    ['a 0x prefix in place of the first byte', `0x${hex.slice(2)}`],
    ['64 bytes', new Uint8Array(64)],
    ['nothing', null],
  ])('refuses %s', (_, input) => {
    expect(() => v.parse(uint8Array32Schema, input)).toThrow('expect 32 bytes')
  })
})

describe('uint8Array64Schema', () => {
  test('takes 64 bytes and 128 hex characters', () => {
    expect(v.parse(uint8Array64Schema, hex + hex)).toEqual(new Uint8Array([...bytes, ...bytes]))
    expect(() => v.parse(uint8Array64Schema, bytes)).toThrow('expect 64 bytes')
  })
})

describe('buffer32Schema', () => {
  test.each([
    ['hex', hex],
    ['a Uint8Array', bytes],
    ['a Buffer', buffer],
  ])('reads %s as a Buffer', (_, input) => {
    const output = v.parse(buffer32Schema, input)

    expect(Buffer.isBuffer(output)).toBe(true)
    expect(output.toString('hex')).toBe(hex)
  })
})

const uuid = '5f2c1d0e-8a4b-4c3d-9e6f-0a1b2c3d4e5f'
const uuidBytes = new Uint8Array(Buffer.from(uuid.replaceAll('-', ''), 'hex'))

describe('uuidv4Schema', () => {
  test.each([
    ['the string', uuid],
    ['the string in upper case', uuid.toUpperCase()],
    ['16 bytes in a Uint8Array', uuidBytes],
    ['16 bytes in a Buffer', Buffer.from(uuidBytes)],
  ])('reads %s as the lowercase string', (_, input) => {
    expect(v.parse(uuidv4Schema, input)).toBe(uuid)
  })

  test('reads what crypto.randomUUID draws', () => {
    const drawn = crypto.randomUUID()
    expect(v.parse(uuidv4Schema, drawn)).toBe(drawn)
  })

  test.each([
    ['a uuid v1', '5f2c1d0e-8a4b-1c3d-9e6f-0a1b2c3d4e5f'],
    ['the wrong variant', '5f2c1d0e-8a4b-4c3d-1e6f-0a1b2c3d4e5f'],
    ['the string without its dashes', uuid.replaceAll('-', '')],
    ['15 bytes', uuidBytes.subarray(1)],
    ['16 bytes that are not a v4', new Uint8Array(16)],
    ['nothing', null],
  ])('refuses %s', (_, input) => {
    expect(() => v.parse(uuidv4Schema, input)).toThrow('expect a uuid v4')
  })
})

const instant = new Date('2026-10-01T12:00:00.000Z')

describe('dateSchema', () => {
  test.each([
    ['a Date', instant],
    ['milliseconds', instant.getTime()],
    ['milliseconds as a decimal string', String(instant.getTime())],
    ['an ISO string', '2026-10-01T12:00:00.000Z'],
    ['a string with an offset', '2026-10-01T14:00:00+02:00'],
  ])('reads %s as the Date', (_, input) => {
    expect(v.parse(dateSchema, input)).toEqual(instant)
  })

  test('hands out a copy of a Date', () => {
    expect(v.parse(dateSchema, instant)).not.toBe(instant)
  })

  test('floors a fraction of a millisecond, before the epoch too', () => {
    expect(v.parse(dateSchema, 1.9).getTime()).toBe(1)
    expect(v.parse(dateSchema, -1.1).getTime()).toBe(-2)
  })

  test('reads 0 as 1970, not as nothing', () => {
    expect(v.parse(dateSchema, 0).getTime()).toBe(0)
  })

  test.each([
    ['a string that is no date', 'yesterday-ish'],
    ['an invalid Date', new Date(Number.NaN)],
    ['not a number', Number.NaN],
    ['nothing', null],
  ])('refuses %s', (_, input) => {
    expect(() => v.parse(dateSchema, input)).toThrow('expect an instant')
  })
})

/**
 * The schema's own pattern against the `uuid` package, which is what legacy checks with:
 * `validate(value) && version(value) === 4`. The package is a dev dependency for this
 * comparison alone.
 */
describe('uuidv4Schema agrees with the uuid package', () => {
  const ours = (value: string) => v.safeParse(uuidv4Schema, value).success
  const theirs = (value: string) => validate(value) && version(value) === 4
  const sample = '5f2c1d0e-8a4b-4c3d-9e6f-0a1b2c3d4e5f'
  const HEX_DIGITS = [...'0123456789abcdef']

  test('on what the package and the runtime draw', () => {
    for (let draw = 0; draw < 1000; draw++) {
      for (const value of [v4(), crypto.randomUUID()]) {
        expect(theirs(value)).toBe(true)
        expect(ours(value)).toBe(true)
      }
    }
  })

  test('on every other version the package generates, and on nil and max', () => {
    const others = [
      v1(),
      v3('gradido.net', v3.DNS),
      v5('gradido.net', v5.DNS),
      v6(),
      v7(),
      NIL,
      MAX,
    ]
    for (const value of others) {
      expect(theirs(value)).toBe(false)
      expect(ours(value)).toBe(false)
    }
  })

  test('on every version digit with every variant digit, in both cases', () => {
    let accepted = 0
    for (const versionDigit of HEX_DIGITS) {
      for (const variantDigit of HEX_DIGITS) {
        const value = `${sample.slice(0, 14)}${versionDigit}${sample.slice(15, 19)}${variantDigit}${sample.slice(20)}`
        for (const cased of [value, value.toUpperCase()]) {
          expect(`${cased} ${ours(cased)}`).toBe(`${cased} ${theirs(cased)}`)
          accepted += ours(cased) ? 1 : 0
        }
      }
    }
    // Version 4 with the variants 8, 9, a and b, each in two cases.
    expect(accepted).toBe(8)
  })

  test.each([
    ['empty', ''],
    ['without dashes', sample.replaceAll('-', '')],
    ['in braces', `{${sample}}`],
    ['as a urn', `urn:uuid:${sample}`],
    ['one character short', sample.slice(1)],
    ['one character long', `${sample}0`],
    ['a letter that is no hex digit', `g${sample.slice(1)}`],
    ['a leading space', ` ${sample}`],
    ['a trailing newline', `${sample}\n`],
    ['a dash moved', '5f2c1d0e8-a4b-4c3d-9e6f-0a1b2c3d4e5f'],
  ])('on a malformed string: %s', (_, value) => {
    expect(theirs(value)).toBe(false)
    expect(ours(value)).toBe(false)
  })
})
