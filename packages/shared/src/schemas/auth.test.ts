import { describe, expect, test } from 'bun:test'
import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import * as v from 'valibot'
import { EMAIL_MAX_LENGTH, emailPrevalidateSchema, emailSchema, loginPasswordSchema } from './auth'

const prevalidate = (value: string) => v.safeParse(emailPrevalidateSchema, value)
const validate = (value: string) => v.safeParse(emailSchema, value)
const messageOf = (result: ReturnType<typeof prevalidate>) =>
  result.success ? undefined : result.issues[0].message

describe('email prevalidation — what is wrong the moment it is typed', () => {
  // The regression this whole split exists for: a `v.trim()` ahead of the whitespace
  // check swallowed the space while it was still trailing, so the field only complained
  // once the next keystroke had turned it into an interior one.
  test('rejects a trailing space, at the keystroke that types it', () => {
    expect(prevalidate('ein ').success).toBe(false)
    expect(messageOf(prevalidate('ein '))).toBe('An email address cannot contain spaces')
  })

  test('rejects whitespace anywhere, including tabs and newlines from a paste', () => {
    for (const value of ['ein horn@x.de', ' ein@x.de', 'a\tb@x.de', 'a@x.de\n']) {
      expect(prevalidate(value).success).toBe(false)
    }
  })

  test('accepts an unfinished address — incomplete is not wrong', () => {
    for (const value of ['', 'e', 'ein', 'einhorn@', 'einhorn@gradido']) {
      expect(prevalidate(value).success).toBe(true)
    }
  })

  test('rejects an address longer than the column it has to fit', () => {
    const local = 'x'.repeat(EMAIL_MAX_LENGTH - '@gradido.net'.length)
    expect(prevalidate(`${local}@gradido.net`).success).toBe(true)
    expect(prevalidate(`${local}x@gradido.net`).success).toBe(false)
    expect(messageOf(prevalidate(`${local}x@gradido.net`))).toBe('This email address is too long')
  })
})

describe('email validation — the full rule', () => {
  test('reports emptiness before format, so a blank field says what it needs', () => {
    expect(messageOf(validate(''))).toBe('This field is required')
    expect(messageOf(validate('abc'))).toBe('Please enter a valid email address')
  })

  test('trims, so an address pasted with padding is accepted and stored clean', () => {
    const result = validate('  einhorn@gradido.net  ')
    expect(result.success).toBe(true)
    expect(result.output).toBe('einhorn@gradido.net')
  })

  // Normalization belongs to validation alone; prevalidation judges the raw input. The
  // two disagreeing on padded input is intended, and `FormField` resolves it by letting
  // a valid value win.
  test('accepts padded input that prevalidation rejects', () => {
    expect(prevalidate(' einhorn@gradido.net ').success).toBe(false)
    expect(validate(' einhorn@gradido.net ').success).toBe(true)
  })
})

describe('login password', () => {
  test('only requires that something was entered', () => {
    expect(v.safeParse(loginPasswordSchema, '').success).toBe(false)
    expect(v.safeParse(loginPasswordSchema, 'x').success).toBe(true)
  })
})

test('EMAIL_MAX_LENGTH matches contracts/db/user_contacts.json', () => {
  const contract = JSON.parse(
    readFileSync(join(import.meta.dir, '../../../../contracts/db/user_contacts.json'), 'utf8'),
  )
  const columns: { name: string; type: string }[] = contract.columns ?? contract.table?.columns
  const email = columns.find((column) => column.name === 'email')
  expect(email?.type).toBe(`varchar(${EMAIL_MAX_LENGTH})`)
})
