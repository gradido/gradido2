import m from 'mithril'
import { t } from '../i18n'

// No Bootstrap JS here: the toasts are rendered as `.toast.show` and dismissed by this
// component's own timer and close button, so its Toast class would only be a second
// thing mutating the DOM mithril owns.
interface Toast {
  id: number
  title: string
  message: string
  variant: string
  bodyClass?: string
  timeout: number
}

const DEFAULT_DELAY = 5000

export class Toaster implements m.ClassComponent {
  private toasts: Toast[] = []
  private counter = 0

  toast(toast: Omit<Toast, 'id' | 'timeout'> & { timeout?: number }): void {
    const id = ++this.counter
    this.toasts.push({ ...toast, id, timeout: toast.timeout ?? DEFAULT_DELAY })
    m.redraw()

    if (toast.timeout !== 0) {
      setTimeout(() => this.removeToast(id), toast.timeout ?? DEFAULT_DELAY)
    }
  }

  removeToast(id: number): void {
    this.toasts = this.toasts.filter((toast) => toast.id !== id)
    m.redraw()
  }

  // Selectors are written out in full so PurgeCSS can find them.
  success(message: string, timeout?: number): void {
    this.toast({ title: t.__('Success'), message, variant: '.text-bg-success', timeout })
  }

  error(message: string, timeout?: number): void {
    this.toast({ title: t.__('Error'), message, variant: '.text-bg-danger', timeout })
  }

  warning(message: string, timeout?: number): void {
    this.toast({
      title: t.__('Info'),
      message,
      variant: '.text-bg-warning',
      bodyClass: '.gdd-toaster-body-darken',
      timeout,
    })
  }

  view() {
    return m(
      '.toast-container.position-fixed.top-0.end-0.p-3',
      this.toasts.map((toast) =>
        m(
          `.toast.show.gdd-toaster${toast.variant}`,
          {
            key: toast.id,
            role: 'alert',
            'aria-live': 'assertive',
            'aria-atomic': true,
          },
          [
            m('.toast-header.gdd-toaster-title', [
              m('strong.me-auto', toast.title),
              m('button.btn-close.ms-2.mb-1', {
                type: 'button',
                'aria-label': t.__('Close'),
                onclick: () => this.removeToast(toast.id),
              }),
            ]),
            m(`.toast-body${toast.bodyClass ?? '.gdd-toaster-body'}`, toast.message),
          ],
        ),
      ),
    )
  }
}

/** One toaster per app — components report through this instead of threading a handle. */
export const toaster = new Toaster()
