import { describe, expect, test } from 'bun:test'
import {
  attrsOf,
  byTag,
  classesOf,
  deepRender,
  find,
  findAll,
} from '@gradido/frontend-core/testing'
import { stubIcons } from '@gradido/frontend-core/testing/icons'

stubIcons()

const { Login } = await import('./Login')
type Login = InstanceType<typeof Login>

const draw = (page: Login) => deepRender(page.view())

const inputs = (page: Login) => findAll(draw(page), byTag('input'))
const submit = (page: Login) =>
  find(draw(page), (vnode) => attrsOf(vnode)['data-test'] === 'submit-login')
const submitWrapper = (page: Login) =>
  find(draw(page), (vnode) => typeof attrsOf(vnode).onmouseenter === 'function')

describe('Login', () => {
  test('opens with both fields neutral', () => {
    const page = new Login()
    for (const input of inputs(page)) {
      expect(classesOf(input)).not.toContain('is-invalid')
    }
  })

  // The handler cannot live on the button itself: a disabled button fires no mouse
  // events, and the button is disabled for exactly as long as the reveal is useful.
  test('reaching for the disabled submit button reveals every problem', () => {
    const page = new Login()
    expect(attrsOf(submit(page)).disabled).toBe(true)
    expect(find(draw(page), (v) => classesOf(v).includes('is-invalid'))).toBeUndefined()
    ;(attrsOf(submitWrapper(page)).onmouseenter as () => void)()
    expect(findAll(draw(page), (v) => classesOf(v).includes('is-invalid'))).toHaveLength(2)
  })

  test('the submit button turns from disabled styling to gradido once the form is valid', () => {
    const page = new Login()
    expect(classesOf(submit(page))).toContain('btn-gradido-disable')

    const fields = (
      page as unknown as { form: { fields: Record<string, { set(v: string): void }> } }
    ).form.fields
    fields.email.set('einhorn@gradido.net')
    fields.password.set('secret')

    expect(classesOf(submit(page))).toContain('btn-gradido')
    expect(attrsOf(submit(page)).disabled).toBe(false)
  })
})
