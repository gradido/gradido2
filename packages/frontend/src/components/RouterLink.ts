import m from 'mithril'

export interface RouterLinkAttrs {
  href: string
  class?: string
  'data-test'?: string
}

/**
 * An in-app link. Wraps `m.route.Link`, which keeps navigation inside the router
 * instead of reloading the document, and marks the link for the current route so the
 * navbar can show where the visitor is.
 */
export const RouterLink: m.Component<RouterLinkAttrs> = {
  view: ({ attrs, children }) => {
    const active = m.route.get()?.split('?')[0] === attrs.href
    const classes = [attrs.class, active ? 'is-active' : ''].filter(Boolean).join(' ')
    return m(
      m.route.Link,
      {
        href: attrs.href,
        class: classes || undefined,
        'data-test': attrs['data-test'],
      },
      children,
    )
  },
}
