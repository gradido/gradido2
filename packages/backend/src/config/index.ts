import { validate } from 'shared/src/config'
import { schema } from './schema'

const auth = {
  JWT_SECRET: process.env.JWT_SECRET ?? 'secret123',
  JWT_EXPIRES_IN: process.env.JWT_EXPIRES_IN ?? '10m',
}

const server = {
  PORT: process.env.PORT ?? 4000,
}

// default log level on production should be info
const LOG_LEVEL = process.env.LOG_LEVEL ?? 'info'

export const CONFIG = {
  ...auth,
  ...server,
  LOG_LEVEL
}

validate(schema, CONFIG)