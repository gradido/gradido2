import { config as dotenvConfig } from 'dotenv'
import { grabEnvAndCheckBySchema } from 'service-core'
import { configSchema } from './schema'

dotenvConfig({ quiet: true })

export const CONFIG = grabEnvAndCheckBySchema(configSchema)
