import { drizzle } from 'drizzle-orm/mysql2'
import mysql from 'mysql2/promise'
import { CONFIG } from '../config'

const poolConnection = mysql.createPool({
  host: CONFIG.DB_HOST,
  port: CONFIG.DB_PORT,
  user: CONFIG.DB_USER,
  password: CONFIG.DB_PASSWORD,
  database: CONFIG.DB_DATABASE,
  connectionLimit: CONFIG.DB_CONNECTION_LIMIT,
})

export const db = drizzle(poolConnection)