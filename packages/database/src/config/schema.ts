import {
  DB_HOST,
  DB_PASSWORD,
  DB_PORT,
  DB_USER,
  DB_DATABASE,
  LOG_LEVEL
} from 'shared/src/config'
import { z } from 'zod'

export const schema = z.object({
  DB_HOST,
  DB_PORT,
  DB_USER,
  DB_PASSWORD,
  DB_DATABASE,
  LOG_LEVEL,
  DB_CONNECTION_LIMIT: z.number().min(1).max(100).default(10).optional()
})