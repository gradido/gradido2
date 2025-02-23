import 'dotenv/config'
import { defineConfig } from 'drizzle-kit'
import { CONFIG } from './src/config'

export default defineConfig({
  out: './drizzle',
  schema: './src/db/schema',
  dialect: 'mysql',
  dbCredentials: {
    host: CONFIG.DB_HOST,
    port: CONFIG.DB_PORT,
    user: CONFIG.DB_USER,
    password: CONFIG.DB_PASSWORD,
    database: CONFIG.DB_DATABASE,
  },
  casing: "snake_case",
})