import { z } from 'zod'
import {
  APP_VERSION,
  BUILD_COMMIT,
  BUILD_COMMIT_SHORT,
  COMMUNITY_DESCRIPTION,
  COMMUNITY_NAME,
  COMMUNITY_SUPPORT_MAIL,
  COMMUNITY_LOCATION,
  COMMUNITY_URL,
  DEBUG,
  DECAY_START_TIME,
  GMS_ACTIVE,
  HUMHUB_ACTIVE,
  NODE_ENV,
  PRODUCTION,
  TRPC_URI,
} from 'shared/config/commonSchema'

export const schema = z.object({
  APP_VERSION,
  BUILD_COMMIT,
  BUILD_COMMIT_SHORT,
  COMMUNITY_DESCRIPTION,
  COMMUNITY_NAME,
  COMMUNITY_SUPPORT_MAIL,
  COMMUNITY_LOCATION,
  COMMUNITY_URL,
  DEBUG,
  DECAY_START_TIME,
  GMS_ACTIVE,
  HUMHUB_ACTIVE,
  NODE_ENV,
  PRODUCTION,
  TRPC_URI,
  ADMIN_AUTH_URL: z.string()
    .url()
    .describe('Extern Url for admin-frontend')
    .default('http://0.0.0.0/admin/authenticate?token='),

  COMMUNITY_REGISTER_URL: z.string()
    .url()
    .describe('URL for Register a new Account in frontend.'),

  FRONTEND_HOSTING: z.enum(['nodejs', 'nginx'])
    .optional()
    .describe('set to `nodejs` if frontend is hosted by vite with a own nodejs instance'),

  FRONTEND_MODULE_URL: z.string()
    .url()
    .optional()
    .describe("Base Url for reaching frontend in browser, only needed if COMMUNITY_URL wasn't set"),

  FRONTEND_MODULE_PROTOCOL: z.string()
    .refine((val:string) => ['http', 'https'].includes(val), {
      message: 'Must be http or https',
    })
    .describe(`
      Protocol for frontend module hosting
      - it has to be the same as for backend api url and admin to prevent mixed block errors,
      - if frontend is served with nodejs:
          is have to be http or setup must be updated to include a ssl certificate
    `)
    .default('http'),

  FRONTEND_MODULE_HOST: z.union([
    z.literal('localhost').describe('Must be localhost'),
    z.string().ip({ version: 'v4' }).describe('Must be a valid IPv4 address'),
    z.string().domain().describe('Must be a valid domain'),
  ])
    .optional()
    .describe('Host (domain, IPv4, or localhost) for the frontend, default is 0.0.0.0 for local hosting during development.')
    .default('0.0.0.0'),

  FRONTEND_MODULE_PORT: z.number()
    .int()
    .min(1024)
    .max(49151)
    .describe('Port for hosting Frontend with Vite as a Node.js instance, default: 3000')
    .default(3000),

  META_URL: z.string()
    .url()
    .describe('The base URL for the meta tags.')
    .default('http://localhost'),

  META_TITLE_DE: z.string()
    .describe('Meta title in German.')
    .default('Gradido – Dein Dankbarkeitskonto'),

  META_TITLE_EN: z.string()
    .describe('Meta title in English.')
    .default('Gradido - Your gratitude account'),

  META_DESCRIPTION_DE: z.string()
    .describe('Meta description in German.')
    .default(
      'Dankbarkeit ist die Währung der neuen Zeit. Immer mehr Menschen entfalten ihr Potenzial und gestalten eine gute Zukunft für alle.',
    ),

  META_DESCRIPTION_EN: z.string()
    .describe('Meta description in English.')
    .default(
      'Gratitude is the currency of the new age. More and more people are unleashing their potential and shaping a good future for all.',
    ),

  META_KEYWORDS_DE: z.string()
    .describe('Meta keywords in German.')
    .default(
      'Grundeinkommen, Währung, Dankbarkeit, Schenk-Ökonomie, Natürliche Ökonomie des Lebens, Ökonomie, Ökologie, Potenzialentfaltung, Schenken und Danken, Kreislauf des Lebens, Geldsystem',
    ),

  META_KEYWORDS_EN: z.string()
    .describe('Meta keywords in English.')
    .default(
      'Basic Income, Currency, Gratitude, Gift Economy, Natural Economy of Life, Economy, Ecology, Potential Development, Giving and Thanking, Cycle of Life, Monetary System',
    ),

  META_AUTHOR: z.string()
    .describe('The author for the meta tags.')
    .default('Bernd Hückstädt - Gradido-Akademie'),
});
