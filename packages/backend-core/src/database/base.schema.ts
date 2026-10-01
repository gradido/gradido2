import * as v from 'valibot'

/**
 * The value types every other schema is built from, in one file.
 *
 * Each takes the value in whichever form the caller holds it and hands it out as the form its
 * name says. The output is what PostgreSQL returns, because PostgreSQL is the reference; the
 * input also admits what SQLite returns where it has no such type -- so a repository parses
 * its rows and nothing after it knows which database they came from.
 */

/** `bigint` identity in PostgreSQL. SQLite hands INTEGER out as a double; ids stay below 2^53. */
export const rowIdSchema = v.pipe(
  v.union([v.bigint(), v.pipe(v.number(), v.safeInteger())]),
  v.transform((id) => BigInt(id)),
)

export type RowIdInput = v.InferInput<typeof rowIdSchema>
export type RowId = v.InferOutput<typeof rowIdSchema>

/**
 * A fixed number of bytes, in whichever form the caller holds them, as the form the name says.
 *
 * ```text
 * accepted, by every schema here     a hex string of 2n characters
 *                                    a Uint8Array of n bytes
 *                                    a Buffer of n bytes
 *
 * uint8Array32Schema, …64Schema      -> a plain Uint8Array: what the code works with
 * buffer32Schema, …64Schema           -> a Buffer: what a `bytea` or `BLOB` column takes
 * ```
 *
 * The output is always a copy. A Buffer's `slice` is a view where a Uint8Array's is a copy, so
 * a Buffer passed on under the type Uint8Array would behave unlike its type.
 */
export type BytesInput = string | Uint8Array

/* Not v.hexadecimal(): it also admits a `0x` prefix, which Buffer.from(…, 'hex') does not read. */
const HEX = /^[0-9a-fA-F]*$/u

function bytesInputSchema(size: number) {
  /* On every check, because a string that fails is reported by its own branch, not the union. */
  const message = `expect ${size} bytes: a Uint8Array, a Buffer or ${size * 2} hex characters`
  return v.union(
    [
      v.pipe(v.string(), v.regex(HEX, message), v.length(size * 2, message)),
      v.custom<Uint8Array>((input) => input instanceof Uint8Array && input.length === size),
    ],
    message,
  )
}

function uint8ArraySchema(size: number) {
  return v.pipe(
    bytesInputSchema(size),
    v.transform(
      (input: BytesInput): Uint8Array =>
        typeof input === 'string'
          ? new Uint8Array(Buffer.from(input, 'hex'))
          : new Uint8Array(input),
    ),
  )
}

function bufferSchema(size: number) {
  return v.pipe(
    bytesInputSchema(size),
    v.transform(
      (input: BytesInput): Buffer =>
        typeof input === 'string' ? Buffer.from(input, 'hex') : Buffer.from(input),
    ),
  )
}

/** 32 bytes at runtime: an ed25519 public key, a hash. */
export const uint8Array32Schema = uint8ArraySchema(32)
/** 64 bytes at runtime: an ed25519 secret key, a signature. */
export const uint8Array64Schema = uint8ArraySchema(64)

/** 32 bytes for a `bytea` / `BLOB` column -- the contract's `bytes(32)`. */
export const buffer32Schema = bufferSchema(32)
/** 64 bytes for a `bytea` / `BLOB` column -- the contract's `bytes(64)`. */
export const buffer64Schema = bufferSchema(64)

const UUIDV4 = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/iu
const UUIDV4_MESSAGE =
  'expect a uuid v4: its 36 characters, or its 16 bytes as a Uint8Array or a Buffer'

/** Version 4 in the high half of byte 6, the RFC 4122 variant in the top two bits of byte 8. */
function isUuidv4Bytes(input: unknown): input is Uint8Array {
  return (
    input instanceof Uint8Array &&
    input.length === 16 &&
    input[6] >> 4 === 4 &&
    input[8] >> 6 === 0b10
  )
}

/**
 * A uuid v4, given as its string or as its 16 bytes, as the lowercase string -- which is what
 * a `uuid` column takes and hands back in PostgreSQL, and what SQLite keeps as text.
 */
export const uuidv4Schema = v.pipe(
  v.union(
    [v.pipe(v.string(), v.regex(UUIDV4, UUIDV4_MESSAGE)), v.custom<Uint8Array>(isUuidv4Bytes)],
    UUIDV4_MESSAGE,
  ),
  v.transform((input: string | Uint8Array): string => {
    if (typeof input === 'string') {
      return input.toLowerCase()
    }
    const hex = Buffer.from(input).toString('hex')
    return [
      hex.slice(0, 8),
      hex.slice(8, 12),
      hex.slice(12, 16),
      hex.slice(16, 20),
      hex.slice(20),
    ].join('-')
  }),
)

export type Uuidv4Input = v.InferInput<typeof uuidv4Schema>
export type Uuidv4 = v.InferOutput<typeof uuidv4Schema>

const DATE_MESSAGE = 'expect an instant: a Date, milliseconds since the Unix epoch or a date string'

/** The wire form of `contracts/types/Timestamp.json`: milliseconds, as a decimal string. */
const MILLISECONDS = /^-?[0-9]+$/u

/**
 * An instant, in whichever form the caller holds it, as a Date -- which is what both column
 * types take: drizzle writes it to `timestamptz` in PostgreSQL and to INTEGER milliseconds in
 * SQLite, and hands a Date back from either.
 *
 * ```text
 * a Date
 * a number                  milliseconds since the Unix epoch, floored
 * a string of digits        the same, as the API carries it
 * any other string          whatever `new Date(string)` reads
 * ```
 *
 * The last one is for values written by hand. A string without a zone is read in the server's
 * local zone, so it is not for anything a request carries.
 */
export const dateSchema = v.pipe(
  v.union([v.date(DATE_MESSAGE), v.number(DATE_MESSAGE), v.string(DATE_MESSAGE)], DATE_MESSAGE),
  v.transform((input: Date | number | string): Date => {
    if (typeof input === 'number') {
      return new Date(Math.floor(input))
    }
    if (typeof input === 'string' && MILLISECONDS.test(input)) {
      return new Date(Number(input))
    }
    return new Date(input)
  }),
  v.check((date) => !Number.isNaN(date.getTime()), DATE_MESSAGE),
)

export type DateInput = v.InferInput<typeof dateSchema>
