import { describe, expect, test } from 'bun:test'
import * as v from 'valibot'
import { Form, FormField } from './FormField'

// Stand-ins for the real schemas, so this tests the state machine rather than valibot.
const noWhitespace = v.pipe(v.string(), v.regex(/^\S*$/u, 'no whitespace'))
const fullRule = v.pipe(v.string(), v.trim(), v.nonEmpty('required'), v.email('bad address'))
const rules = { prevalidate: noWhitespace, validate: fullRule }

const field = (value = '') => new FormField(rules, value)

describe('state while the user is still typing', () => {
  test('is neutral when the value is merely unfinished', () => {
    expect(field('').state).toBeUndefined()
    expect(field('ein').state).toBeUndefined()
    expect(field('einhorn@').state).toBeUndefined()
  })

  test('turns red as soon as prevalidation fails, without waiting for a blur', () => {
    expect(field('ein ').state).toBe(false)
  })

  test('turns green as soon as the full rule passes', () => {
    expect(field('einhorn@gradido.net').state).toBe(true)
  })

  // Prevalidation judges the raw value and rejects the padding; validation trims it away
  // and passes. Valid has to win, or a pasted address would be reported for whitespace
  // that is about to be removed.
  test('a valid value wins over a prevalidation complaint', () => {
    const padded = field(' einhorn@gradido.net ')
    expect(v.safeParse(noWhitespace, padded.value).success).toBe(false)
    expect(padded.state).toBe(true)
  })
})

describe('the three reveal triggers', () => {
  test('leaving the field shows what is outstanding', () => {
    const email = field('')
    expect(email.state).toBeUndefined()
    email.touch()
    expect(email.state).toBe(false)
  })

  test('reveal() shows it too — used on submit and on reaching the submit button', () => {
    const email = field('')
    email.reveal()
    expect(email.state).toBe(false)
  })

  test('neither turns a valid field red', () => {
    const email = field('einhorn@gradido.net')
    email.touch()
    email.reveal()
    expect(email.state).toBe(true)
  })

  test('reset() puts the field back to neutral', () => {
    const email = field('')
    email.touch()
    email.reveal()
    email.reset()
    expect(email.state).toBeUndefined()
  })
})

describe('which message is shown', () => {
  // The knock-on "bad address" used to mask the reason the field went red. The message
  // has to name what prevalidation objected to, because that is what the user can fix.
  test('prevalidation explains the field it turned red', () => {
    expect(field('ein h').issue).toBe('no whitespace')
  })

  test('otherwise the full rule speaks', () => {
    expect(field('').issue).toBe('required')
    expect(field('abc').issue).toBe('bad address')
  })

  test('a valid field has nothing to say', () => {
    expect(field('einhorn@gradido.net').issue).toBeUndefined()
  })
})

describe('values', () => {
  test('parsed applies the normalization the full rule describes', () => {
    expect(field('  einhorn@gradido.net  ').parsed).toBe('einhorn@gradido.net')
  })

  test('set() records exactly what was typed', () => {
    const email = field()
    email.set('ein ')
    expect(email.value).toBe('ein ')
  })

  test('a bare schema means no prevalidation — nothing is wrong while typing', () => {
    const password = new FormField(v.pipe(v.string(), v.nonEmpty('required')))
    expect(password.state).toBeUndefined()
    password.set('x')
    expect(password.state).toBe(true)
  })
})

describe('Form', () => {
  const build = () =>
    new Form({
      email: new FormField(rules),
      password: new FormField(v.pipe(v.string(), v.nonEmpty('required'))),
    })

  test('is valid only once every field is', () => {
    const form = build()
    expect(form.valid).toBe(false)
    form.fields.email.set('einhorn@gradido.net')
    expect(form.valid).toBe(false)
    form.fields.password.set('secret')
    expect(form.valid).toBe(true)
  })

  test('reveal() shows every outstanding problem at once', () => {
    const form = build()
    expect(Object.values(form.fields).map((f) => f.state)).toEqual([undefined, undefined])
    form.reveal()
    expect(Object.values(form.fields).map((f) => f.state)).toEqual([false, false])
  })

  test('values() hands over the normalized values', () => {
    const form = build()
    form.fields.email.set('  einhorn@gradido.net ')
    form.fields.password.set('secret')
    expect(form.values()).toEqual({ email: 'einhorn@gradido.net', password: 'secret' })
  })

  test('reset() clears values and state', () => {
    const form = build()
    form.fields.email.set('x')
    form.reveal()
    form.reset()
    expect(form.fields.email.value).toBe('')
    expect(form.fields.email.state).toBeUndefined()
  })
})
