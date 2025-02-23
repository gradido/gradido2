import m, { Children, ClassComponent, Vnode } from 'mithril'
import Popover from 'bootstrap/js/src/popover'

interface BPopoverAttrs {
  id: string
  title: Children
  content: string
  darkMode: boolean
  trigger?: 'click' | 'hover' | 'focus' // click = default
  placement?: 'top' | 'bottom' | 'left' | 'right' // top = default
}

export class BPopover implements ClassComponent<BPopoverAttrs> {
  oncreate(vnode: Vnode<BPopoverAttrs>) {
    const trigger = document.getElementById(vnode.attrs.id)
    const content = document.getElementById(vnode.attrs.id + '-content')
    if (trigger && content) {
      trigger.addEventListener('show.bs.popover', () => {
        content.style.display = 'block'
      })
    }
    new Popover(trigger, {
      container: '#' + vnode.attrs.id,
      content,
      html: true,
      trigger: vnode.attrs.trigger || 'click',
      placement: vnode.attrs.placement || 'top',
    })
  }

  view(vnode: Vnode<BPopoverAttrs>) {
    const popoverClass = vnode.attrs.darkMode ? '.b-popover-dark' : ''
    return [
      m('#' + vnode.attrs.id + '.pointer' + popoverClass, {
        'data-bs-toggle': 'popover',
      }, vnode.attrs.title),
      m('#' + vnode.attrs.id + '-content.popover.b-popover' + popoverClass + '.bs-popover-top', { style: { display: 'none' }}, vnode.children)
      /*m('#' + vnode.attrs.id + '.pointer', {
        'data-bs-toggle': 'popover',
        'data-bs-placement': vnode.attrs.placement || 'top',
        'data-bs-content': vnode.attrs.content,
      }, vnode.attrs.title),
      m('.popover.b-popover' + popoverClass + '.fade',
        { role: 'tooltip' }, 
        vnode.children
      )*/
    ]
  }
}