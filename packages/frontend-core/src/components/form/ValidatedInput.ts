import m from 'mithril'
import type { FormField } from '../../form/FormField'
import { translateIssue } from '../../i18n'

export interface ValidatedInputAttrs {
  field: FormField
  name: string
  label: string
  placeholder?: string
  type?: string
  inputmode?: string
  autocomplete?: string
  disabled?: boolean
  /** Added to the field's wrapper — spacing is the arranging form's business. */
  class?: string
  /** Rendered after the input inside an input group, e.g. a show-password toggle. */
  append?: m.Children
  /** Called after every accepted keystroke, so a form can re-evaluate its submit button. */
  oninput?: (value: string) => void
}

const validityClass = (state: boolean | undefined): string => {
  if (state === undefined) {
    return ''
  }
  return state ? '.is-valid' : '.is-invalid'
}

/** A labelled input that renders its own validation state and message. */
export const ValidatedInput: m.Component<ValidatedInputAttrs> = {
  view: ({ attrs }) => {
    const { field, name, label, append, disabled } = attrs
    const id = `${name}-input-field`
    const feedbackId = `${name}-feedback`
    const issue = field.state === false ? field.issue : undefined

    const input = m(`input.form-control${validityClass(field.state)}`, {
      id,
      name,
      type: attrs.type ?? 'text',
      inputmode: attrs.inputmode,
      value: field.value,
      placeholder: attrs.placeholder ?? label,
      autocomplete: attrs.autocomplete,
      disabled,
      'data-test': `input-${name}`,
      'aria-invalid': field.state === false,
      'aria-describedby': feedbackId,
      oninput: (event: Event) => {
        const value = (event.target as HTMLInputElement).value
        field.set(value)
        attrs.oninput?.(value)
      },
      onblur: () => field.touch(),
    })

    const feedback = m('.invalid-feedback', { id: feedbackId }, issue ? translateIssue(issue) : '')

    return m(`.input-${name}`, { class: attrs.class }, [
      m('label.form-label', { for: id }, label),
      append ? m('.input-group.has-validation', [input, append, feedback]) : [input, feedback],
    ])
  },
}
