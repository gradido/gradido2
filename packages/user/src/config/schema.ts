import { z } from 'zod'
import { LOG_LEVEL } from 'shared/src/config'

export const schema = z.object({
  LOG_LEVEL,
  JWT_SECRET: z.string()
    .default('secret123')
    .describe('jwt secret for jwt tokens used for login'),

  JWT_EXPIRES_IN: z.union([
    z.string()
      .regex(/^\d+[smhdw]$/) // Time specification such as “10m”, “1h”, “2d”, etc.
      .describe('Expiration time for JWT login token, in format like "10m", "1h", "1d"')
      .default('10m'),
    z.number()
      .positive() // positive number to accept seconds
      .describe('Expiration time for JWT login token in seconds'),
  ])
  .describe('Time for JWT token to expire, auto logout'),
})
