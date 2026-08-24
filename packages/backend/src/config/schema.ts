import { dbSchema } from 'backend-core'
import { envPort, serviceSchema } from 'service-core'
import * as v from 'valibot'

export const configSchema = v.object({
  ...serviceSchema.entries,
  ...dbSchema.entries,
  BACKEND_PORT: envPort('4000'),
})
