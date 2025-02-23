import './assets/scss/gradido.scss'

import m from 'mithril'
import i18nInit from './i18nInit'
import { Toaster } from 'shared/src/components/Toaster'
import { createTRPCProxyClient, httpBatchLink } from '@trpc/client'
import type { authRouter } from 'backend'
import LoginPage from './pages/LoginPage'
import DashboardPage from './pages/DashboardPage'

i18nInit()
globalThis.toaster = new Toaster
globalThis.client = createTRPCProxyClient<authRouter>({
  links: [httpBatchLink({ url: 'http://localhost:3000/trpc' })],
  url: 'http://localhost:3000/trpc',
  headers: () => {
    const token = localStorage.getItem('token')
    return token ? { Authorization: `Bearer ${token}` } : {}
  },
})
localStorage.setItem('language', navigator.language)
var root = document.getElementById('app')!

m.route.prefix = ''

// routes
m.route(root, '/', {
  '/': {
    onmatch: () => m.route.set('/dashboard'),
  },
  '/login': LoginPage,
  '/dashboard': DashboardPage,
})

