import { describe, expect, test } from 'bun:test'
import { attrsOf, byTag, classesOf, deepRender, find, findAll, textOf } from 'frontend-core/testing'
import { stubIcons } from 'frontend-core/testing/icons'

stubIcons()
const { Register } = await import('./Register')
type Register = InstanceType<typeof Register>

const draw = (page: Register) => deepRender(page.view())
const submit = (page: Register) =>
  find(draw(page), (vnode) => attrsOf(vnode)['data-test'] === 'submit-register')
const submitWrapper = (page: Register) =>
  find(draw(page), (vnode) => typeof attrsOf(vnode).onmouseenter === 'function')
const invalidFields = (page: Register) =>
  findAll(draw(page), (vnode) => classesOf(vnode).includes('is-invalid'))

const fill = (page: Register) => {
  const fields = (
    page as unknown as { form: { fields: Record<string, { set(value: unknown): void }> } }
  ).form.fields
  fields.firstName.set('Einhorn')
  fields.lastName.set('Immond')
  fields.email.set('einhorn@gradido.net')
  fields.privacy.set(true)
}

describe('Register', () => {
  test('asks for a first name, a last name and an address', () => {
    const page = new Register()
    const ids = findAll(draw(page), byTag('input')).map((input) => attrsOf(input).id)
    expect(ids).toEqual([
      'firstname-input-field',
      'lastname-input-field',
      'email-input-field',
      'privacy-checkbox',
    ])
  })

  test('opens with nothing marked wrong', () => {
    expect(invalidFields(new Register())).toHaveLength(0)
  })

  test('reaching for the disabled button reveals every outstanding field', () => {
    const page = new Register()
    expect(attrsOf(submit(page)).disabled).toBe(true)
    ;(attrsOf(submitWrapper(page)).onmouseenter as () => void)()
    expect(invalidFields(page)).toHaveLength(4)
  })

  // Consent is the one field a visitor can miss while everything else looks finished.
  test('stays disabled until the privacy policy is agreed to', () => {
    const page = new Register()
    fill(page)
    expect(attrsOf(submit(page)).disabled).toBe(false)

    const fields = (
      page as unknown as { form: { fields: Record<string, { set(v: unknown): void }> } }
    ).form.fields
    fields.privacy.set(false)
    expect(attrsOf(submit(page)).disabled).toBe(true)
  })

  test('the button turns from disabled styling to gradido once the form is complete', () => {
    const page = new Register()
    expect(classesOf(submit(page))).toContain('btn-gradido-disable')
    fill(page)
    expect(classesOf(submit(page))).toContain('btn-gradido')
  })

  // Legacy stretches the login button across its column but not this one.
  test('the button is not stretched across its column', () => {
    expect(classesOf(submit(new Register()))).not.toContain('w-100')
  })

  // Only reachable once the backend exists, so a test is the only thing watching it.
  test('replaces the form with a note about the activation email once registered', () => {
    const page = new Register()
    ;(page as unknown as { registered: boolean }).registered = true

    // No catalog is loaded under test, so `t.__` yields the English source strings.
    expect(findAll(draw(page), byTag('input'))).toHaveLength(0)
    expect(textOf(draw(page))).toContain('Thank you!')
    expect(textOf(draw(page))).toContain('click the activation link')
  })

  test('links to the privacy policy from the consent label', () => {
    const href = attrsOf(find(draw(new Register()), byTag('a'))).href
    expect(String(href)).toContain('/datenschutz/')
  })
})
