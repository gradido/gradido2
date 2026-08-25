import { grabEnvAndCheckBySchema } from '@gradido/service-core'
import { config as dotenvConfig } from 'dotenv'
import { configSchema } from './schema'

dotenvConfig({ quiet: true })

export const CONFIG = grabEnvAndCheckBySchema(configSchema)
