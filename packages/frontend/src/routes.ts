export const ROUTES = {
  login: '/login',
  register: '/register',
  forgotPassword: '/forgot-password',
} as const

export type RouteName = keyof typeof ROUTES
