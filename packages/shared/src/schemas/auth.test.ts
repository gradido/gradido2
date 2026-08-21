import { describe, expect, test } from 'bun:test'
import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import * as v from 'valibot'
import {
  EMAIL_MAX_LENGTH,
  emailPrevalidateSchema,
  emailSchema,
  firstNameSchema,
  lastNameSchema,
  loginPasswordSchema,
  NAME_MAX_LENGTH,
  privacyConsentSchema,
} from './auth'

const prevalidate = (value: string) => v.safeParse(emailPrevalidateSchema, value)
const validate = (value: string) => v.safeParse(emailSchema, value)
// Widened to either schema's result: the two pipes produce different issue unions, so a
// helper typed against one of them cannot be handed the other's.
const messageOf = (result: ReturnType<typeof prevalidate> | ReturnType<typeof validate>) =>
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

describe('names', () => {
  test('a first name needs three characters, a last name two', () => {
    expect(messageOf(v.safeParse(firstNameSchema, 'Ei'))).toBe(
      'Please enter at least three characters',
    )
    expect(v.safeParse(firstNameSchema, 'Ein').success).toBe(true)
    expect(messageOf(v.safeParse(lastNameSchema, 'L'))).toBe('Please enter at least two characters')
    expect(v.safeParse(lastNameSchema, 'Li').success).toBe(true)
  })

  test('an empty name says what it needs rather than how short it is', () => {
    expect(messageOf(v.safeParse(firstNameSchema, ''))).toBe('This field is required')
  })

  test('surrounding whitespace is trimmed away, not counted', () => {
    expect(v.parse(firstNameSchema, '  Einhorn  ')).toBe('Einhorn')
    expect(v.safeParse(firstNameSchema, '  Ei  ').success).toBe(false)
  })

  test('a name may not outgrow the column that stores it', () => {
    expect(v.safeParse(firstNameSchema, 'E'.repeat(NAME_MAX_LENGTH)).success).toBe(true)
    expect(v.safeParse(firstNameSchema, 'E'.repeat(NAME_MAX_LENGTH + 1)).success).toBe(false)
  })
})

describe('privacy consent', () => {
  test('has to be given', () => {
    expect(messageOf(v.safeParse(privacyConsentSchema, false))).toBe(
      'Please agree to the privacy policy',
    )
    expect(v.safeParse(privacyConsentSchema, true).success).toBe(true)
  })
})

test('NAME_MAX_LENGTH matches contracts/db/users.json', () => {
  const contract = JSON.parse(
    readFileSync(join(import.meta.dir, '../../../../contracts/db/users.json'), 'utf8'),
  )
  const columns: { name: string; type: string }[] = contract.columns
  for (const name of ['first_name', 'last_name']) {
    expect(columns.find((column) => column.name === name)?.type).toBe(`varchar(${NAME_MAX_LENGTH})`)
  }
})

test('EMAIL_MAX_LENGTH matches contracts/db/user_contacts.json', () => {
  const contract = JSON.parse(
    readFileSync(join(import.meta.dir, '../../../../contracts/db/user_contacts.json'), 'utf8'),
  )
  const columns: { name: string; type: string }[] = contract.columns ?? contract.table?.columns
  const email = columns.find((column) => column.name === 'email')
  expect(email?.type).toBe(`varchar(${EMAIL_MAX_LENGTH})`)
})
