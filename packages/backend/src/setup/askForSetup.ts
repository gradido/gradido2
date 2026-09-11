import {
  DATABASE_PASSWORD_MESSAGE,
  type DatabaseConfig,
  databaseConfigSchema,
  isDatabasePasswordAcceptable,
} from '@gradido/backend-core'
import {
  type EmailConfig,
  emailConfigSchema,
  portSchema,
  type SecretVariable,
  type SmtpTlsMode,
  secretSource,
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
import { DOCKER_COMPOSE_POSTGRES_PORT, findPostgres, POSTGRES_DEFAULT_PORT } from './findPostgres'
import { askChoice, askSecret, askText, askYesNo, type Choice, isTerminal, say } from './prompt'

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
 * **The database is asked first**, because it is the one answer that depends on this machine
 * rather than on the person at it: SQLite needs nothing, PostgreSQL needs a server that is
 * already running. So the conversation looks before it asks — see `findPostgres.ts` — and a
 * database that is configured already is offered as it stands, so that a rerun of `setup` to
 * change the relay does not end up somewhere else.
 *
 * The question after it decides how many of the others are asked. A development installation
 * is a known thing — this machine and the MailDev container of the repository's
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
  /** The same variables, parsed — what opens the database and what probes the relay. The
   *  database is the configured one where the answers kept it, see `databaseFrom`. */
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

/** The password `docker-compose.yml` starts its PostgreSQL with when the environment names
 *  none — `${DB_PASSWORD:-gradido}` there. User and database have the same defaults as the
 *  configuration, so they need no constant of their own. */
const DOCKER_COMPOSE_POSTGRES_PASSWORD = 'gradido'

export async function askForSetup(): Promise<SetupAnswers> {
  say(
    '',
    'This database has no community yet.',
    '',
    'What follows is written once and becomes this instance’s identity: other',
    'communities will know it by these answers and by a key pair generated here.',
    'The database and the mail relay are written to .env beside it.',
  )

  const database = await askForDatabase()

  const production = await askChoice<boolean>(
    'What kind of installation is this?',
    [
      {
        value: false,
        label: 'development',
        hint: 'localhost, mail into the MailDev container',
      },
      { value: true, label: 'production', hint: 'every value is asked for' },
    ],
    CONFIG.NODE_ENV === 'production' ? 1 : 0,
  )
  if (production) {
    await insistOnProductionPassword(database)
  }

  if (!production) {
    const proposal = developmentSetup(database)
    say('', 'These are the development settings:', '')
    for (const line of describe(proposal)) {
      say(`  ${line}`)
    }
    await reportRelay(proposal.email, true)
    if (await askYesNo('Take them?', true)) {
      return proposal
    }
  }

  return askEveryField(production, database)
}

/** Whether anybody is there to be asked. The caller turns a no into a line saying what to
 *  do instead — see `setupCommand.ts`. */
export function canAskForSetup(): boolean {
  return isTerminal()
}

/**
 * What a developer's machine is, without asking — beyond the database, which was asked.
 *
 * `http://localhost` because that is what a checkout runs as, and the MailDev container
 * because it is the mail sink this repository ships: nothing is delivered, every mail is
 * readable at `http://localhost:1081`, and there are no credentials because the container is
 * started without any.
 */
function developmentSetup(database: Readonly<Record<string, string>>): SetupAnswers {
  return answersFrom(
    {
      name: DEVELOPMENT_COMMUNITY_NAME,
      description: null,
      url: DEVELOPMENT_COMMUNITY_URL,
    },
    {
      ...database,
      NODE_ENV: 'development',
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

async function askEveryField(
  production: boolean,
  database: Readonly<Record<string, string>>,
): Promise<SetupAnswers> {
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

  const env: Record<string, string> = {
    ...database,
    NODE_ENV: production ? 'production' : 'development',
  }
  await askForEmail(env, community)

  const answers = answersFrom(community, env)
  await reportRelay(answers.email, false)
  return answers
}

/**
 * Which database, and where — the variables to write for it, or none at all when what is
 * configured already is kept.
 *
 * Kept means not written: the lines that configure it stay exactly as they are, wherever
 * they came from, and `answersFrom` reads them back out of the configuration.
 */
async function askForDatabase(): Promise<Record<string, string>> {
  const found = await reportPostgres()
  const configured = isDatabaseConfigured()
  if (configured) {
    say('', 'The database is configured already:', `  ${describeDatabase(CONFIG)}`)
  }
  say(
    '',
    'SQLite keeps the whole database in one file beside the server and needs nothing',
    'else. PostgreSQL is the reference, for a community with an administrator; it is a',
    'server of its own, and it has to be installed and running before this one starts.',
  )

  type Answer = 'configured' | 'sqlite' | 'postgresql'
  const choices: Choice<Answer>[] = [
    ...(configured
      ? [{ value: 'configured' as const, label: 'as configured', hint: 'the database above' }]
      : []),
    { value: 'sqlite', label: 'sqlite', hint: 'one file, nothing to install' },
    {
      value: 'postgresql',
      label: 'postgresql',
      hint: 'a server that has to be installed and running',
    },
  ]
  /* What is configured before what is running: a rerun is somebody changing one thing, and a
     server that happens to be up is not a reason to move their data somewhere else. */
  const proposed: Answer = configured ? 'configured' : found.length !== 0 ? 'postgresql' : 'sqlite'
  const chosen = await askChoice<Answer>(
    'Which database?',
    choices,
    choices.findIndex((choice) => choice.value === proposed),
  )

  if (chosen === 'configured') {
    return {}
  }
  if (chosen === 'sqlite') {
    return {
      DB_TYPE: 'sqlite',
      DB_FILE: await askText('Database file', CONFIG.DB_FILE, requiredTextSchema),
    }
  }
  return askForPostgres(found)
}

/**
 * Says which of the two ports a PostgreSQL answers on, and answers with those that do.
 *
 * Something that answers and is not PostgreSQL is said as well: it is the reason a server
 * pointed at that port would fail, and finding that out now is cheaper than at the first start.
 */
async function reportPostgres(): Promise<number[]> {
  say('', 'Looking for PostgreSQL on this machine …')
  const ports = await findPostgres()
  for (const { port, answer } of ports) {
    const where = `localhost:${port}`.padEnd(16)
    const what =
      answer === 'postgresql'
        ? 'PostgreSQL answers'
        : answer === 'other'
          ? 'something answers, and it is not PostgreSQL'
          : 'nothing answers'
    say(`  ${where} ${what} — ${portMeaning(port)}`)
  }
  return ports.filter(({ answer }) => answer === 'postgresql').map(({ port }) => port)
}

function portMeaning(port: number): string {
  return port === POSTGRES_DEFAULT_PORT
    ? 'the port PostgreSQL listens on unless told otherwise'
    : 'the port docker-compose.yml publishes it on'
}

/**
 * The questions for PostgreSQL, with what they propose depending on what was found.
 *
 * What is configured, when it is PostgreSQL already — somebody who chose it over keeping it is
 * changing one value. Otherwise, when the container of `docker-compose.yml` answers, what that
 * container was started with: its port, and the user, password and database it reads from the
 * same variables — or their defaults. A server on 5432 changes nothing about the proposal: the
 * default is already the socket directory of a server on this machine, which is where that one
 * is fastest, see `contracts/database-config.json`, rules.connection.
 */
async function askForPostgres(found: readonly number[]): Promise<Record<string, string>> {
  const container = CONFIG.DB_TYPE !== 'postgresql' && found.includes(DOCKER_COMPOSE_POSTGRES_PORT)
  const proposedPassword =
    container && CONFIG.DB_PASSWORD === '' ? DOCKER_COMPOSE_POSTGRES_PASSWORD : CONFIG.DB_PASSWORD

  const env: Record<string, string> = { DB_TYPE: 'postgresql' }
  env.DB_HOST = await askText(
    'Database host',
    container ? 'localhost' : CONFIG.DB_HOST,
    requiredTextSchema,
  )
  env.DB_PORT = await askText(
    'Database port',
    String(container ? DOCKER_COMPOSE_POSTGRES_PORT : CONFIG.DB_PORT),
    portTextSchema,
  )
  env.DB_DATABASE = await askText('Database name', CONFIG.DB_DATABASE, requiredTextSchema)
  env.DB_USER = await askText('Database user', CONFIG.DB_USER, requiredTextSchema)
  if (!keepsItsOwnSecret('DB_PASSWORD')) {
    env.DB_PASSWORD = await askSecret(
      'Database password',
      proposedPassword,
      proposedPassword === CONFIG.DB_PASSWORD
        ? undefined
        : `${proposedPassword}, what docker-compose.yml sets`,
    )
  }
  return env
}

/**
 * The rule `config/schema.ts` applies at every start, applied here instead: a password refused
 * now costs one more question, refused at the next start it costs a server that will not come
 * up. Asked of the database as it will be — answered or kept — because a kept one is held to
 * the same rule, and only a production installation is asked it at all.
 */
async function insistOnProductionPassword(database: Record<string, string>): Promise<void> {
  for (;;) {
    const config = { ...databaseFrom(database), NODE_ENV: 'production' }
    if (isDatabasePasswordAcceptable(config) || keepsItsOwnSecret('DB_PASSWORD')) {
      return
    }
    process.stderr.write(`  ${DATABASE_PASSWORD_MESSAGE}\n`)
    database.DB_PASSWORD = await askSecret('Database password', '')
  }
}

/**
 * Whether the environment names a database already — out of `.env`, where `setup` wrote it the
 * last time, or out of whatever started the process.
 *
 * Any variable that says *which* database counts. The password and the pool size do not: on
 * their own they describe no database, only how to talk to one.
 */
function isDatabaseConfigured(): boolean {
  return ['DB_TYPE', 'DB_FILE', 'DB_HOST', 'DB_PORT', 'DB_DATABASE', 'DB_USER'].some(
    (name) => (process.env[name] ?? '') !== '',
  )
}

/** A database on one line: what keeping it says, and what the development block shows. */
function describeDatabase(database: DatabaseConfig): string {
  return database.DB_TYPE === 'sqlite'
    ? `sqlite in ${database.DB_FILE}`
    : `postgresql, ${database.DB_DATABASE} on ${database.DB_HOST}:${database.DB_PORT} as ${database.DB_USER}`
}

/**
 * The database the answers name: what is configured, with what was answered on top.
 *
 * Both, because an answer may be only part of it — nothing when the configured database is
 * kept, no password when one comes from a systemd credential or a file — and the part that was
 * not answered is still configured, in the form the configuration read it in.
 */
function databaseFrom(answered: Readonly<Record<string, string>>): DatabaseConfig {
  return v.parse(databaseConfigSchema, {
    DB_TYPE: CONFIG.DB_TYPE,
    DB_FILE: CONFIG.DB_FILE,
    DB_HOST: CONFIG.DB_HOST,
    DB_PORT: String(CONFIG.DB_PORT),
    DB_USER: CONFIG.DB_USER,
    DB_PASSWORD: CONFIG.DB_PASSWORD,
    DB_DATABASE: CONFIG.DB_DATABASE,
    DB_POOL_SIZE: String(CONFIG.DB_POOL_SIZE),
    ...answered,
  })
}

/**
 * Whether this secret comes from somewhere `setup` must not touch, and says so if it does.
 *
 * The question exists because of what this command does with an answer: it offers the current
 * value as the default and writes what comes back into `.env`. That is right for a value that
 * was in the environment and wrong for one that was not — pressing Enter on a password systemd
 * keeps on a tmpfs, or one that lives in a file with its own permissions, would copy it into a
 * file in the working directory. A mechanism that can be downgraded by answering a prompt is
 * not one, so the prompt is not asked. See contracts/secrets.json.
 */
function keepsItsOwnSecret(name: SecretVariable): boolean {
  const source = secretSource(name)
  if (source !== 'credential' && source !== 'file') {
    return false
  }
  say(
    `  ${name} comes from ${source === 'credential' ? 'a systemd credential' : `the file ${name}_FILE names`} — leaving it alone`,
  )
  return true
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
  if (env.EMAIL_USERNAME === '') {
    env.EMAIL_PASSWORD = ''
  } else if (!keepsItsOwnSecret('EMAIL_PASSWORD')) {
    env.EMAIL_PASSWORD = await askSecret('SMTP password', CONFIG.EMAIL_PASSWORD)
  }
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
    database: databaseFrom(env),
    email: v.parse(emailConfigSchema, env),
  }
}

/** What the development proposal looks like on screen. The passwords are empty there and
 *  are shown as such; a proposal with a secret in it is not one this command makes. */
function describe(answers: SetupAnswers): string[] {
  return [
    `community name  ${answers.community.name}`,
    `public URL      ${answers.community.url}`,
    `database        ${describeDatabase(answers.database)}`,
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
