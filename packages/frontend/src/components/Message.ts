import m from 'mithril'
import { RouterLink } from './RouterLink'

export interface MessageAttrs {
  headline: string
  subtitle: string
  buttonText?: string
  linkTo?: string
}

/** Full-card message that replaces a form when it cannot be shown any more. */
export const Message: m.Component<MessageAttrs> = {
  view: ({ attrs }) =>
    m('.text-center.py-4', [
      m('p.h1', { 'data-test': 'message-headline' }, attrs.headline),
      m('p.h4', { 'data-test': 'message-subtitle' }, attrs.subtitle),
      m('hr'),
      attrs.buttonText && attrs.linkTo
        ? m(
            RouterLink,
            { href: attrs.linkTo, class: 'btn btn-gradido', 'data-test': 'message-button' },
            attrs.buttonText,
          )
        : null,
    ]),
}
