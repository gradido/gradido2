import { randomBytes } from 'node:crypto'

/**
 * `user_contacts.email_verification_code` — 53 random bits, never zero.
 *
 * The width is not a security decision, it is the widest value that survives both databases
 * unchanged. Legacy draws 64 bits into a MariaDB `bigint unsigned`; PostgreSQL `bigint` and
 * SQLite `INTEGER` are both *signed*, so the top bit is gone before anything is stored, and
 * SQLite hands an INTEGER to JavaScript as a double, which quietly rounds anything past
 * 2^53-1. A code that comes back changed is a link that does not work and a row that cannot
 * be found — with nothing failing anywhere. `contracts/db/user_contacts.json` records the
 * bound.
 *
 * What is left is 9.0e15 codes against a window measured in hours, drawn from the system CSPRNG.
 * The scarce thing here was never the entropy.
 *
 * Zero is excluded because it is what an unset column looks like, and a code nobody was sent
 * must not match a row.
 */
export function newEmailVerificationCode(): bigint {
  /* Seven bytes is 56 bits, masked down to the low 53. A mask keeps every value equally
     likely, which a modulo would not; the loop is only here to exclude zero. */
  for (;;) {
    const value = BigInt(`0x${randomBytes(7).toString('hex')}`) & 0x1fffffffffffffn
    if (value !== 0n) {
      return value
    }
  }
}
