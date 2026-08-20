import type m from 'mithril'

// Mithril vnodes are plain objects, so a component's markup can be asserted on without a
// DOM: call its `view` and inspect what comes back. Keeps component tests in `bun test`
// with no browser environment to configure.

type AnyVnode = m.Vnode<Record<string, unknown>, unknown>

/** Depth-first walk over a vnode tree. Fragments (`tag: '['`) are descended into too. */
export function* walk(node: unknown): Generator<AnyVnode> {
  if (node === null || typeof node !== 'object') {
    return
  }
  if (Array.isArray(node)) {
    for (const child of node) {
      yield* walk(child)
    }
    return
  }
  const vnode = node as AnyVnode
  yield vnode
  yield* walk(vnode.children)
}

export const findAll = (root: unknown, predicate: (v: AnyVnode) => boolean): AnyVnode[] =>
  [...walk(root)].filter(predicate)

export const find = (root: unknown, predicate: (v: AnyVnode) => boolean): AnyVnode | undefined =>
  [...walk(root)].find(predicate)

export const byTag =
  (tag: string) =>
  (vnode: AnyVnode): boolean =>
    vnode.tag === tag

/** The classes mithril parsed out of the selector, e.g. `input.form-control.is-valid`. */
export const classesOf = (vnode: AnyVnode | undefined): string[] =>
  String((vnode?.attrs as { className?: string } | undefined)?.className ?? '')
    .split(' ')
    .filter(Boolean)

export const attrsOf = (vnode: AnyVnode | undefined): Record<string, unknown> =>
  (vnode?.attrs ?? {}) as Record<string, unknown>

/** The text a subtree renders. Mithril wraps every string child in a `#` text vnode. */
export const textOf = (root: unknown): string =>
  [...walk(root)]
    .filter((vnode) => vnode.tag === '#')
    .map((vnode) => String(vnode.children ?? ''))
    .join('')

/** Render a stateless component's view without mounting it. */
export const render = <A>(component: m.Component<A>, attrs: A): m.Children =>
  // `view` is allowed to return void; nothing under test does, and a caller wants markup.
  component.view({ attrs } as m.Vnode<A>) as m.Children

/**
 * Resolve a tree down to plain markup, rendering nested components as it goes.
 *
 * `m(SomeComponent, …)` is a vnode whose tag *is* the component, so a component that
 * delegates its markup to another one produces no `input` or `button` to assert on until
 * the nested one has run. Nested components are rendered fresh, which is what a test of
 * the outer component's state wants: the state under test lives in the instance the test
 * holds.
 */
export function deepRender(node: unknown): unknown {
  if (node === null || typeof node !== 'object') {
    return node
  }
  if (Array.isArray(node)) {
    return node.map(deepRender)
  }

  const vnode = node as AnyVnode
  const tag: unknown = vnode.tag

  if (typeof tag === 'function') {
    const ClassComponent = tag as new (v: AnyVnode) => { view: (v: AnyVnode) => m.Children }
    return deepRender(new ClassComponent(vnode).view(vnode))
  }
  if (typeof tag === 'object' && tag !== null && 'view' in tag) {
    const component = tag as m.Component<Record<string, unknown>, unknown>
    return deepRender(component.view(vnode as Parameters<typeof component.view>[0]))
  }
  return { ...vnode, children: deepRender(vnode.children) }
}
