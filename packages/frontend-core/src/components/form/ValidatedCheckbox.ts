import m from 'mithril'
import type { FormField } from '../..'
import { translateIssue } from '../../i18n'

export interface ValidatedCheckboxAttrs {
  field: FormField<boolean>
  name: string
  /** Children rather than a string: a consent label usually carries a link. */
  label: m.Children
  disabled?: boolean
  onchange?: (checked: boolean) => void
}

/**
 * A checkbox that reports its own validation state.
 *
 * Consent has no half-typed state, so there is nothing to prevalidate — it turns red
 * only once the visitor leaves it or reaches for the submit button.
 */
export const ValidatedCheckbox: m.Component<ValidatedCheckboxAttrs> = {
  view: ({ attrs }) => {
    const { field, name, label, disabled } = attrs
    const id = `${name}-checkbox`
    const feedbackId = `${name}-feedback`
    const invalid = field.state === false

    return m(`.form-check.input-${name}`, [
      m(`input.form-check-input${invalid ? '.is-invalid' : ''}`, {
        id,
        name,
        type: 'checkbox',
        checked: field.value,
        disabled,
        'data-test': `input-${name}`,
        'aria-invalid': invalid,
        'aria-describedby': feedbackId,
        onchange: (event: Event) => {
          const checked = (event.target as HTMLInputElement).checked
          field.set(checked)
          attrs.onchange?.(checked)
        },
        onblur: () => field.touch(),
      }),
      m('label.form-check-label', { for: id }, label),
      m('.invalid-feedback', { id: feedbackId }, invalid ? translateIssue(field.issue ?? '') : ''),
    ])
  },
}
