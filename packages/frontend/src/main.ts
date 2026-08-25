import './styles/index.scss'

import { setLocale, storedLocale, t } from '@gradido/frontend-core'
import m from 'mithril'
import { AuthLayout } from './layouts'
import { Login, Placeholder, Register } from './pages'
import { ROUTES } from './routes'

const root = document.getElementById('app')
if (!root) {
  throw new Error('#app not found')
}

// Real paths, not hash routes — `/login` has to keep working as a bookmark, and the
// dev server and nginx both fall back to index.html.
m.route.prefix = import.meta.env.BASE_URL.replace(/\/$/, '')

// The catalog is loaded before the first render, so nothing flashes in English first.
// Titles are read inside `render` rather than here, so switching language re-translates
// them instead of freezing whatever was current when the routes were defined.
setLocale(storedLocale(), import.meta.env.BASE_URL).then(() => {
  m.route(root, ROUTES.login, {
    [ROUTES.login]: {
      render: () => m(AuthLayout, m(Login)),
    },
    [ROUTES.register]: {
      render: () => m(AuthLayout, m(Register)),
    },
    [ROUTES.forgotPassword]: {
      render: () => m(AuthLayout, m(Placeholder, { title: t.__('Reset password') })),
    },
  })
})
