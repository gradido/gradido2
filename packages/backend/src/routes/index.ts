import { initTRPC } from '@trpc/server'

const t = initTRPC.create()
const procedure = t.procedure
const router = t.router

export { t, procedure, router }