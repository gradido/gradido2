import m from 'mithril'

export interface BNavItemAttrs extends m.Attributes {
  href?: string
  target?: string
  classes?: string[]
}

export const BNavItem: m.Component<BNavItemAttrs> = {
  view: ({ attrs, children }) => {
    const { href, target, classes, ...rest } = attrs
    const isCurrentPage = href && m.route.get() === href
    return m(
      'li.nav-item.auth-navbar', { class: classes?.join('.') },
      isCurrentPage
        ? m('span.nav-link', { ...rest }, children)
        : m('a.nav-link', { href: href ?? '#', target, ...rest }, children)
    )
  },
}