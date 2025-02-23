
import { validate } from 'shared/src/config'
import { schema } from './schema'
import dotenv from 'dotenv'

dotenv.config()

const database = {
  DB_HOST: process.env.DB_HOST || 'localhost',
  DB_PORT: process.env.DB_PORT ? parseInt(process.env.DB_PORT) : 3306,
  DB_USER: process.env.DB_USER || 'root',
  DB_PASSWORD: process.env.DB_PASSWORD || undefined,
  DB_DATABASE: process.env.DB_DATABASE || 'gradido_community',
  DB_CONNECTION_LIMIT: process.env.DB_CONNECTION_LIMIT ? parseInt(process.env.DB_CONNECTION_LIMIT) : 10,
}

// default log level on production should be info
const LOG_LEVEL = process.env.LOG_LEVEL ?? 'info'

export const CONFIG = { ...database, LOG_LEVEL }

validate(schema, CONFIG)