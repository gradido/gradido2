import { initTRPC } from '@trpc/server'
import {
  CreateHTTPContextOptions,
} from '@trpc/server/adapters/standalone'
import { z } from 'zod'
import { TRPCError } from '@trpc/server'

// Initialize a context for the server
function createContext(opts: CreateHTTPContextOptions) {
  return {}
}

// Get the context type
type Context = Awaited<ReturnType<typeof createContext>>

// Initialize tRPC
const t = initTRPC.context<Context>().create()

// Create main router
// Define a user type
type User = {
  id: string
  username: string
  password: string
}

// Mock user database
const users: User[] = []

// Secret key for JWT
const JWT_SECRET = 'your_secret_key'

// Middleware to check authentication
const isAuthenticated = t.middleware(({ ctx, next }) => {
  const token = ctx.req.headers.authorization?.split(' ')[1]
  if (!token) {
    throw new TRPCError({ code: 'UNAUTHORIZED' })
  }

  try {
    const secret = new TextEncoder().encode(JWT_SECRET)
    const decoded = jwtVerify(token, secret) as User
    ctx.user = decoded
    return next()
  } catch {
    throw new TRPCError({ code: 'UNAUTHORIZED' })
  }
})

// Create main router
const appRouter = t.router({
  register: t.procedure
    .input(z.object({
      username: z.string(),
      password: z.string(),
    }))
    .mutation(({ input }) => {
      const { username, password } = input
      const user = { id: Date.now().toString(), username, password }
      users.push(user)
      const token = SignJWT(user, JWT_SECRET)
      return { token }
    }),
  login: t.procedure
    .input(z.object({
      username: z.string(),
      password: z.string(),
    }))
    .mutation(({ input }) => {
      const { username, password } = input
      const user = users.find(u => u.username === username && u.password === password)
      if (!user) {
        throw new TRPCError({ code: 'UNAUTHORIZED' })
      }
      const token = SignJWT(user, JWT_SECRET)
      return { token }
    }),
  getUser: t.procedure
    .use(isAuthenticated)
    .query(({ ctx }) => {
      return ctx.user
    }),
})

// Export the app router type to be imported on the client side
export type AppRouter = typeof appRouter