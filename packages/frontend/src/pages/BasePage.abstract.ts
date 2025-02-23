import m from 'mithril'
import { DefaultLayout } from '../layouts/DefaultLayout'
import { AuthService } from '../services/AuthService'

export abstract class BasePage<T = {}> implements m.ClassComponent<T> {
  // Standard-Layout
  protected layout: m.Component = DefaultLayout
  protected publicPage: boolean = false

  protected setLayout(layout: m.Component): void {
    this.layout = layout
  }

  protected setPublicPage(publicPage: boolean): void {
    this.publicPage = publicPage
  }

  protected checkAuth(): void {
    if (!AuthService.isAuthenticated()) {
      m.route.set('/login');
    }
  }

  oninit(vnode: m.Vnode<T>): void {
    if (!this.publicPage) {
      this.checkAuth()
    }
  }

  view(vnode: m.Vnode<T>): m.Children {
    return [
      m(this.layout, vnode.children),
      m('.goldrand.position-fixed.fixed-bottom.zindex1000')
    ]
  }
}