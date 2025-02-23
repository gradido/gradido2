import m from 'mithril'

export const DefaultLayout: m.Component = {
  view(vnode) {
    return m('div', [
      m('header', 'Standard-Header'),
      m('main', vnode.children),
      m('footer', 'Standard-Footer'),
    ])
  },
}