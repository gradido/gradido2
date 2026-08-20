import * as v from 'valibot'
import { configSchema, NodeEnvironmentType } from './schema'

// Vite replaces `process.env.<NAME>` at build time only for static property accesses,
// so every variable has to be spelled out here rather than looped over.
const raw = {
  NODE_ENV: process.env.NODE_ENV,
  API_BASE_URL: process.env.API_BASE_URL,
  BASE_PATH: process.env.BASE_PATH,
  COMMUNITY_NAME: process.env.COMMUNITY_NAME,
  WEBSITE_URL: process.env.WEBSITE_URL,
  DEV_SERVER_PORT: process.env.DEV_SERVER_PORT,
}

function parseConfig() {
  try {
    return v.parse(configSchema, raw)
  } catch (error) {
    if (error instanceof v.ValiError) {
      const issue = error.issues[0]
      throw new Error(`config ${String(issue.path?.[0]?.key)}: ${issue.message}`)
    }
    throw error
  }
}

export const CONFIG = parseConfig()
export const PRODUCTION = CONFIG.NODE_ENV === NodeEnvironmentType.Production
