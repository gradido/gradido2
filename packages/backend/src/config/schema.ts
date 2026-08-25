import { dbSchema } from '@gradido/backend-core'
import { envPort, serviceSchema } from '@gradido/service-core'
import * as v from 'valibot'

export const configSchema = v.object({
  ...serviceSchema.entries,
  ...dbSchema.entries,
  BACKEND_PORT: envPort('4000'),
})
