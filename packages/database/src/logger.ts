import options from 'shared/config/log4js'
import { CONFIG } from './config'
import { configure, getLogger } from 'log4js'

options.categories.db.level = CONFIG.LOG_LEVEL

configure(options)

export const logger = getLogger('db')
export class LogError extends Error {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  constructor(msg: string, ...details: any[]) {
    super(msg)
    logger.error(msg, ...details)
  }
}
