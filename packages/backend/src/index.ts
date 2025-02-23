import { Elysia } from 'elysia'
import { cors } from '@elysiajs/cors'
import { swagger } from '@elysiajs/swagger'
import { trpc } from '@elysiajs/trpc'


import { authRouter } from './routes/authRouter'
import { CONFIG } from './config'

new Elysia()
  .use(cors())
  .use(trpc(authRouter))
  .use(swagger())
  .listen(CONFIG.PORT)

export { type authRouter } from './routes/authRouter'