import { t } from 'frontend-core'
import m from 'mithril'
import { Message } from '../components/Message'
import { ROUTES } from '../routes'

export interface PlaceholderAttrs {
  title: string
}

/** Stands in for an auth page that has not been rebuilt yet, so its links are not dead. */
export const Placeholder: m.Component<PlaceholderAttrs> = {
  view: ({ attrs }) =>
    m(Message, {
      headline: attrs.title,
      subtitle: t.__('This page has not been built yet.'),
      buttonText: t.__('Sign in'),
      linkTo: ROUTES.login,
    }),
}
