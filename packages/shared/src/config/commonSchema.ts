import { z } from 'zod'

const urlSchema = z.string().url();

export const browserUrls = z.array(urlSchema)
  .refine((urls) => {
    let protocol: string | undefined;
    for (const url of urls) {
      const urlObject = new URL(url);
      if (!protocol) {
        protocol = urlObject.protocol;
      } else if (urlObject.protocol !== protocol) {
        return false;
      }
    }
    return true;
  }, {
    message: 'All URLs need to have the same protocol to prevent mixed block errors'
  })
  .describe('All URLs need to have same protocol to prevent mixed block errors');

export const DECAY_START_TIME = z.string()
  .datetime({ precision: 4 })
  .default('2021-05-13T17:46:31Z')
  .describe('The start time for decay, expected in ISO 8601 format (e.g. 2021-05-13T17:46:31Z)');

export const DB_VERSION = z.string()
  .regex(/^\d{4}-[a-z0-9-_]+$/, 'DB_VERSION must be in the format: YYYY-description, e.g. "0087-add_index_on_user_roles".')
  .describe('db version string, last migration file name without ending or last folder in entity');

export const COMMUNITY_URL = z.string()
  .url()
  .refine((value) => !value.endsWith('/'), {
    message: 'URL should not end with a slash (/)',
  })
  .default('http://0.0.0.0')
  .describe('The base URL of the community, should have the same protocol as frontend, admin and backend api to prevent mixed content issues.');

export const GRAPHQL_URI = z.string()
  .url()
  .default('http://0.0.0.0/graphql')
  .describe('The external URL of the backend service, accessible from outside the server (e.g., via Nginx or the server\'s public URL), should have the same protocol as frontend and admin to prevent mixed content issues.');

export const COMMUNITY_NAME = z.string()
  .min(3)
  .max(40)
  .default('Gradido Entwicklung')
  .describe('The name of the community');

export const COMMUNITY_DESCRIPTION = z.string()
  .min(10)
  .max(255)
  .default('Die lokale Entwicklungsumgebung von Gradido.')
  .describe('A short description of the community');

export const COMMUNITY_SUPPORT_MAIL = z.string()
  .email()
  .default('support@supportmail.com')
  .describe('The support email address for the community will be used in frontend and E-Mails');

export const COMMUNITY_LOCATION = z.string()
  .regex(/^[-+]?[0-9]{1,2}(\.[0-9]+)?,\s?[-+]?[0-9]{1,3}(\.[0-9]+)?$/)
  .default('49.280377, 9.690151')
  .describe('Geographical location of the community in "latitude, longitude" format');

export const GRAPHIQL = z.boolean()
  .default(false)
  .describe('Flag for enabling GraphQL playground for debugging.');

export const GMS_ACTIVE = z.boolean()
  .default(false)
  .describe('Flag to indicate if the GMS (Geographic Member Search) service is used.');

export const GDT_ACTIVE = z.boolean()
  .default(false)
  .describe('Flag to indicate if the GMS (Geographic Member Search) service is used.');

export const GDT_API_URL = z.string()
  .url()
  .default('https://gdt.gradido.net')
  .describe('The URL for GDT API endpoint');

export const HUMHUB_ACTIVE = z.boolean()
  .default(false)
  .describe('Flag to indicate if the HumHub based Community Server is used.');

export const LOG_LEVEL = z.enum(['all', 'mark', 'trace', 'debug', 'info', 'warn', 'error', 'fatal', 'off'])
  .default('info')
  .describe('set log level');

export const LOG4JS_CONFIG = z.string()
  .regex(/^[a-zA-Z0-9-_]+\.json$/, 'LOG4JS_CONFIG must be a valid filename ending with .json')
  .default('log4js-config.json')
  .describe('config file name for log4js config file');

export const LOGIN_APP_SECRET = z.string()
  .regex(/^[a-fA-F0-9]+$/, 'need to be valid hex')
  .default('21ffbbc616fe')
  .describe('App secret for salt component for libsodium crypto_pwhash');

export const LOGIN_SERVER_KEY = z.string()
  .regex(/^[a-fA-F0-9]{32}$/, 'need to be valid hex and 32 character')
  .default('a51ef8ac7ef1abf162fb7a65261acd7a')
  .describe('Server key for password hashing as additional salt for libsodium crypto_shorthash_keygen');

export const TRPC_URI = z.string()
  .url()
  .default('http://0.0.0.0/trcp')
  .describe('The external URL of the backend service, accessible from outside the server (e.g., via Nginx or the server\'s public URL), should have the same protocol as frontend and admin to prevent mixed content issues.');

export const TYPEORM_LOGGING_RELATIVE_PATH = z.string()
  .regex(/^[a-zA-Z0-9-_\./]+\.log$/, 'TYPEORM_LOGGING_RELATIVE_PATH must be a valid filename ending with .log')
  .default('typeorm.log')
  .describe('log file name for logging typeorm activities');

export const DB_HOST = z.string()
  .regex(/^[a-zA-Z0-9.-]+$/, 'must be a valid host with alphanumeric characters, numbers, points and -')
  .default('localhost')
  .describe("database host like 'localhost' or 'mariadb' in docker setup");

export const DB_PORT = z.number()
  .int()
  .min(1024)
  .max(49151)
  .default(3306)
  .describe('database port, default: 3306');

export const DB_USER = z.string()
  .regex(/^[A-Za-z0-9]([A-Za-z0-9-_\.]*[A-Za-z0-9])?$/, 'Valid database username (letters, numbers, hyphens, underscores, dots allowed; no spaces, must not start or end with hyphen, dot, or underscore)')
  .min(1)
  .max(16)
  .default('root')
  .describe('database username for mariadb');

export const DB_PASSWORD = z.string()
  .refine((value) => {
    if (process.env.NODE_ENV === 'development') {
      return true;
    }
    return /^(?=.*[a-z])(?=.*[A-Z])(?=.*\d)(?=.*[!@#$%^&*(),.?":{}|<>]).{8,32}$/.test(value);
  }, {
    message: 'Password must be between 8 and 32 characters long, and contain at least one uppercase letter, one lowercase letter, one number, and one special character (e.g., !@#$%^&*).'
  })
  .default('')
  .describe('Password for the database user. In development mode, an empty password is allowed. In other environments, a complex password is required.');

export const DB_DATABASE = z.string()
  .regex(/^[a-zA-Z][a-zA-Z0-9_-]{1,63}$/, 'Database name like gradido_community (must start with a letter, and can only contain letters, numbers, underscores, or dashes)')
  .default('gradido_community')
  .describe('Database name like gradido_community (must start with a letter, and can only contain letters, numbers, underscores, or dashes)');

export const APP_VERSION = z.string()
  .regex(/^\d+\.\d+\.\d+$/, 'Version must be in the format "major.minor.patch" (e.g., "2.4.1")')
  .describe('App Version from package.json, all modules share one version');

export const BUILD_COMMIT = z.string()
  .regex(/^[0-9a-f]{40}$/, 'The commit hash must be a 40-character hexadecimal string.')
  .optional()
  .describe('The full git commit hash.');

export const BUILD_COMMIT_SHORT = z.string()
  .regex(/^[0-9a-f]{7}$/, 'The first 7 hexadecimal character from git commit hash.')
  .describe('A short version from the git commit hash.');

export const NODE_ENV = z.enum(['production', 'development', 'test'])
  .default('development')
  .describe('Specifies the environment in which the application is running.');

export const DEBUG = z.boolean()
  .default(false)
  .describe('Indicates whether the application is in debugging mode. Set to true when NODE_ENV is not "production".');

export const PRODUCTION = z.boolean()
  .default(false)
  .describe('Indicates whether the application is running in production mode. Set to true when NODE_ENV is "production".');
