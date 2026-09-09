import { type DatabaseConfig, databaseConfigSchema } from '@gradido/backend-core'
import {
  type EmailConfig,
  emailConfigSchema,
  portSchema,
  type SmtpTlsMode,
  smtpRelay,
} from '@gradido/service-core'
import {
  communityDescriptionSchema,
  communityNameSchema,
  communityUrlSchema,
  emailSchema,
  type HomeCommunitySetup,
} from '@gradido/shared/schemas'
import * as v from 'valibot'
import { CONFIG } from '../config'
import { probeMailRelay } from '../mail'
import { askChoice, askSecret, askText, askYesNo, isTerminal, say } from './prompt'

/**
 * The `setup` command's conversation: what this installation is, and where it sends its mail.
 *
 * Two kinds of answer come out of it and they are kept apart on purpose. The community's
 * name, description and URL are its **identity** and become a row — written once, referred
 * to afterwards, and carrying a key pair that could never have come from an env file. The
 * database and the relay are **configuration**: they are needed before there is a database
 * to read them out of, so they go into `.env`. `contracts/settings.json` draws the same line
 * and says why neither belongs in the settings table.
 *
 * The first question decides how many of the others are asked. A development installation is
 * a known thing — this machine, one SQLite file, the MailDev container of the repository's
 * `docker-compose.yml` — so it is proposed as a block and taken with one Enter. Everything
 * else is asked field by field, with what is configured now in parentheses, so that a second
 * run of `setup` is a way to change one value rather than a way to retype eleven.
 */

/** What the whole conversation produced. */
export type SetupAnswers = {
  /** The row. */
  readonly community: HomeCommunitySetup
  /** The variables, as text, exactly as they go into `.env`. */
  readonly env: Readonly<Record<string, string>>
  /** The same variables, parsed — what opens the database and what probes the relay. */
  readonly database: DatabaseConfig
  readonly email: EmailConfig
}

export const DEVELOPMENT_COMMUNITY_NAME = 'Gradido Dev Community'
export const DEVELOPMENT_COMMUNITY_URL = 'http://localhost'
/** The relay of the repository's `docker-compose.yml` — `MAILDEV_SMTP_PORT`, one above
 *  maildev's own 1025 so that a checkout of legacy can be up at the same time. */
export const MAILDEV_SMTP_PORT = '1026'
/** And the web interface that shows what arrived — `MAILDEV_WEB_PORT`. */
export const MAILDEV_WEB_PORT = '1081'

export async function askForSetup(): Promise<SetupAnswers> {
  say(
    '',
    'This database has no community yet.',
    '',
    'What follows is written once and becomes this instance’s identity: other',
    'communities will know it by these answers and by a key pair generated here.',
    'The database and the mail relay are written to .env beside it.',
  )

  const production = await askChoice<boolean>(
    'What kind of installation is this?',
    [
      {
        value: false,
        label: 'development',
        hint: 'localhost, SQLite, mail into the MailDev container',
      },
      { value: true, label: 'production', hint: 'every value is asked for' },
    ],
    CONFIG.NODE_ENV === 'production' ? 1 : 0,
  )

  if (!production) {
    const proposal = developmentSetup()
    say('', 'These are the development settings:', '')
    for (const line of describe(proposal)) {
      say(`  ${line}`)
    }
    await reportRelay(proposal.email, true)
    if (await askYesNo('Take them?', true)) {
      return proposal
    }
  }

  return askEveryField(production)
}

/** Whether anybody is there to be asked. The caller turns a no into a line saying what to
 *  do instead — see `setupCommand.ts`. */
export function canAskForSetup(): boolean {
  return isTerminal()
}

/**
 * What a developer's machine is, without asking.
 *
 * `http://localhost` and SQLite because that is what a checkout runs as, and the MailDev
 * container because it is the mail sink this repository ships: nothing is delivered, every
 * mail is readable at `http://localhost:1081`, and there are no credentials because the
 * container is started without any.
 */
function developmentSetup(): SetupAnswers {
  return answersFrom(
    {
      name: DEVELOPMENT_COMMUNITY_NAME,
      description: null,
      url: DEVELOPMENT_COMMUNITY_URL,
    },
    {
      NODE_ENV: 'development',
      DB_TYPE: 'sqlite',
      DB_FILE: CONFIG.DB_FILE,
      EMAIL: 'true',
      EMAIL_SMTP_HOST: 'localhost',
      EMAIL_SMTP_PORT: MAILDEV_SMTP_PORT,
      /* MailDev speaks no TLS and asks for no password. Anything else here would be a
         handshake it answers with a refusal. */
      EMAIL_SMTP_TLS: 'none',
      EMAIL_USERNAME: '',
      EMAIL_PASSWORD: '',
      EMAIL_SENDER: 'dev@gradido.localhost',
      EMAIL_SENDER_NAME: DEVELOPMENT_COMMUNITY_NAME,
    },
  )
}

async function askEveryField(production: boolean): Promise<SetupAnswers> {
  say(
    '',
    'Every question shows what it would take in parentheses; Enter takes it,',
    'anything else replaces it.',
    '',
  )

  const community: HomeCommunitySetup = {
    name: await askText('Community name', DEVELOPMENT_COMMUNITY_NAME, communityNameSchema),
    description: await askText('Description (optional)', '', communityDescriptionSchema),
    url: await askText('Public URL, e.g. https://gdd.example.org', '', communityUrlSchema),
  }

  const env: Record<string, string> = { NODE_ENV: production ? 'production' : 'development' }
  await askForDatabase(env, production)
  await askForEmail(env, community)

  const answers = answersFrom(community, env)
  await reportRelay(answers.email, false)
  return answers
}

async function askForDatabase(env: Record<string, string>, production: boolean): Promise<void> {
  const type = await askChoice<'sqlite' | 'postgresql'>(
    'Which database?',
    [
      { value: 'sqlite', label: 'sqlite', hint: 'one file, no service to run' },
      {
        value: 'postgresql',
        label: 'postgresql',
        hint: 'the reference, for a community with an administrator',
      },
    ],
    CONFIG.DB_TYPE === 'postgresql' ? 1 : 0,
  )
  env.DB_TYPE = type

  if (type === 'sqlite') {
    env.DB_FILE = await askText('Database file', CONFIG.DB_FILE, requiredTextSchema)
    return
  }

  env.DB_HOST = await askText('Database host', CONFIG.DB_HOST, requiredTextSchema)
  env.DB_PORT = await askText('Database port', String(CONFIG.DB_PORT), portTextSchema)
  env.DB_DATABASE = await askText('Database name', CONFIG.DB_DATABASE, requiredTextSchema)
  env.DB_USER = await askText('Database user', CONFIG.DB_USER, requiredTextSchema)
  for (;;) {
    env.DB_PASSWORD = await askSecret('Database password', CONFIG.DB_PASSWORD)
    /* The rule `config/schema.ts` applies at every start, applied here instead — a password
       refused now costs one more question, and refused at the next start it costs a server
       that will not come up. */
    if (!production || env.DB_PASSWORD !== '') {
      return
    }
    process.stderr.write('  an empty database password is not acceptable in production\n')
  }
}

async function askForEmail(
  env: Record<string, string>,
  community: HomeCommunitySetup,
): Promise<void> {
  const sends = await askYesNo('Does this instance send mail?', CONFIG.EMAIL)
  env.EMAIL = String(sends)
  if (!sends) {
    return
  }

  env.EMAIL_SMTP_HOST = await askText('SMTP host', CONFIG.EMAIL_SMTP_HOST, requiredTextSchema)
  env.EMAIL_SMTP_PORT = await askText('SMTP port', String(CONFIG.EMAIL_SMTP_PORT), portTextSchema)
  env.EMAIL_SMTP_TLS = await askChoice<SmtpTlsMode>(
    'How is the session to the relay encrypted?',
    [
      { value: 'require', label: 'require', hint: 'STARTTLS, and refuse the relay without it' },
      { value: 'starttls', label: 'starttls', hint: 'STARTTLS where offered, plain where not' },
      { value: 'implicit', label: 'implicit', hint: 'TLS from the first byte, which is port 465' },
      { value: 'none', label: 'none', hint: 'plain, for a relay on this machine' },
    ],
    ['require', 'starttls', 'implicit', 'none'].indexOf(CONFIG.EMAIL_SMTP_TLS),
  )
  env.EMAIL_USERNAME = await askText(
    'SMTP user, empty for a relay that wants none',
    CONFIG.EMAIL_USERNAME,
    optionalTextSchema,
  )
  /* Not asked when there is no user to go with it: SMTP AUTH is a pair, and a password on
     its own authenticates as nobody. */
  env.EMAIL_PASSWORD =
    env.EMAIL_USERNAME === '' ? '' : await askSecret('SMTP password', CONFIG.EMAIL_PASSWORD)
  env.EMAIL_SENDER = await askText('Sender address', CONFIG.EMAIL_SENDER, emailSchema)
  env.EMAIL_SENDER_NAME = await askText(
    'Sender name',
    CONFIG.EMAIL_SENDER_NAME === '' ? community.name : CONFIG.EMAIL_SENDER_NAME,
    optionalTextSchema,
  )
}

/**
 * Opens a session to the relay the answers name and says whether it answered.
 *
 * The same probe the backend runs at every start, run once here while the person who typed
 * the address is still standing at the terminal — a wrong port found now is a correction,
 * found at the next start it is a search. @p maildev adds the one sentence that is only ever
 * true of a development setup: the container is part of this repository and starting it is
 * one command.
 */
async function reportRelay(email: EmailConfig, maildev: boolean): Promise<void> {
  if (!email.EMAIL) {
    return
  }
  const relay = smtpRelay(email)
  say('', `Asking ${relay.host}:${relay.port} whether it takes mail …`)
  const failure = await probeMailRelay(relay)
  if (failure === undefined) {
    say('  it does.')
    if (maildev) {
      say(`  What it receives is readable at http://localhost:${MAILDEV_WEB_PORT}.`)
    }
    return
  }
  say(`  it did not answer: ${failure}`)
  if (maildev) {
    say(
      '  That is the MailDev container of this repository, and it is not running.',
      '  Start it with: docker compose up -d maildev',
      '  The setup can be finished either way — nothing here needs it.',
    )
  }
}

/** The answers as the three things the caller needs them as. */
function answersFrom(
  community: HomeCommunitySetup,
  env: Readonly<Record<string, string>>,
): SetupAnswers {
  return {
    community,
    env,
    /* Parsed rather than assembled a second time, so a value that reaches the database is a
       value that survived the same schema a start would have read it with. Both schemas
       ignore the variables that are not theirs. */
    database: v.parse(databaseConfigSchema, env),
    email: v.parse(emailConfigSchema, env),
  }
}

/** What the development proposal looks like on screen. The passwords are empty there and
 *  are shown as such; a proposal with a secret in it is not one this command makes. */
function describe(answers: SetupAnswers): string[] {
  return [
    `community name  ${answers.community.name}`,
    `public URL      ${answers.community.url}`,
    `database        ${answers.env.DB_TYPE} in ${answers.env.DB_FILE}`,
    `mail relay      ${answers.env.EMAIL_SMTP_HOST}:${answers.env.EMAIL_SMTP_PORT}, no TLS, no login`,
    `sender          ${answers.env.EMAIL_SENDER_NAME} <${answers.env.EMAIL_SENDER}>`,
  ]
}

const requiredTextSchema = v.pipe(v.string(), v.trim(), v.nonEmpty('This field is required'))

const optionalTextSchema = v.pipe(v.string(), v.trim())

/** A port, kept as the text it will be written to `.env` as, checked by the rule that will
 *  read it back. */
const portTextSchema = v.pipe(
  v.string(),
  v.trim(),
  v.check(
    (typed: string) => v.safeParse(portSchema, typed).success,
    'This must be a port between 1 and 65535',
  ),
)
