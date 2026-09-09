import * as v from 'valibot'
import { flagSchema, portSchema } from '../config/schema'

/**
 * How the session to the relay is encrypted.
 *
 * One knob rather than a scheme and a flag, because the two are not independent: `implicit`
 * is TLS from the first byte and puts `smtps://` in the URL, where the other three are a
 * plain connection that may or may not be upgraded afterwards. Spelling that as two
 * variables invites the combination that means nothing — `smtps://` with STARTTLS required.
 *
 * The numbers are what `sc_mail_relay.starttls` carries on the fast path; see
 * `fast-servers/service-core/include/service_core/email/transport.h`.
 *
 *   none      plain, and it stays plain
 *   starttls  upgrade if the relay offers it, carry on in the clear if it does not
 *   require   upgrade, and refuse the session if the relay cannot
 *   implicit  TLS before the greeting, which is what port 465 means
 */
export const SMTP_TLS_MODES = ['none', 'starttls', 'require', 'implicit'] as const

export type SmtpTlsMode = (typeof SMTP_TLS_MODES)[number]

export const SMTP_TLS_MESSAGE = `This must be one of: ${SMTP_TLS_MODES.join(', ')}`

/**
 * Where this instance sends its mail, from the environment.
 *
 * The names are legacy's — `EMAIL`, `EMAIL_USERNAME`, `EMAIL_SENDER`, `EMAIL_PASSWORD`,
 * `EMAIL_SMTP_HOST` and `EMAIL_SMTP_PORT` are what a `stage5.env` holds — so that an
 * operator moving a community to gradido2 does not have to relearn them. `EMAIL_SMTP_TLS`
 * and `EMAIL_SENDER_NAME` are new: legacy let nodemailer guess the encryption from the port
 * number, and guessing is how a password ends up on the wire in the clear.
 *
 * It is env and not the settings table for the reason `contracts/settings.json` gives: the
 * password is a credential, and a value whose leak is an incident is not a setting. It is
 * also needed before there is a database to read a setting out of.
 *
 * `EMAIL=false` is a working configuration and the default. A community that has not been
 * told where to send mail sends none, rather than failing to start.
 */
export const emailConfigSchema = v.object({
  EMAIL: v.optional(flagSchema, 'false'),
  EMAIL_SMTP_HOST: v.optional(v.string(), 'localhost'),
  EMAIL_SMTP_PORT: v.optional(portSchema, '587'),
  EMAIL_SMTP_TLS: v.optional(v.picklist(SMTP_TLS_MODES, SMTP_TLS_MESSAGE), 'starttls'),
  /* Both or neither: SMTP AUTH is a pair, and half of one is a session that authenticates
     as nobody. A relay that wants no credentials — a MailDev in a compose stack — gets two
     empty strings. */
  EMAIL_USERNAME: v.optional(v.string(), ''),
  EMAIL_PASSWORD: v.optional(v.string(), ''),
  /* The envelope sender, and the From: header. */
  EMAIL_SENDER: v.optional(v.string(), ''),
  /* The display name in front of it. Empty sends the bare address. */
  EMAIL_SENDER_NAME: v.optional(v.string(), ''),
})

export type EmailConfig = v.InferOutput<typeof emailConfigSchema>

export const EMAIL_SENDER_MESSAGE = 'EMAIL=true needs a sender address in EMAIL_SENDER'
export const EMAIL_HOST_MESSAGE = 'EMAIL=true needs a relay in EMAIL_SMTP_HOST'

/**
 * Whether this configuration could send a mail if it were asked to.
 *
 * Two rules that each need `EMAIL` as well as the field they are about, which is why they
 * are predicates over the whole config rather than pipes on the fields: an empty
 * `EMAIL_SENDER` is the default and is correct — right up to the moment somebody writes
 * `EMAIL=true` and expects mail to arrive. Applied by the service's own schema the way
 * `isDatabasePasswordAcceptable` is; see `packages/backend/src/config/schema.ts`.
 */
export function hasEmailSender(config: EmailConfig): boolean {
  return !config.EMAIL || config.EMAIL_SENDER !== ''
}

export function hasEmailHost(config: EmailConfig): boolean {
  return !config.EMAIL || config.EMAIL_SMTP_HOST !== ''
}

/**
 * Where and how to reach the relay — `sc_mail_relay` on the fast path, and what the mailer of
 * `@gradido/email-native` is configured from.
 *
 * `url` and `starttls` are the one knob `EMAIL_SMTP_TLS` taken apart the way curl wants it: the
 * scheme carries implicit TLS, and the number is the upgrade to attempt on a plain connection.
 * Deriving it here is what keeps the two from being spelled differently in two places.
 */
export type SmtpRelay = {
  /** `smtp://host:port`, or `smtps://host:port` for `implicit`. What curl is handed there. */
  readonly url: string
  readonly host: string
  readonly port: number
  readonly tls: SmtpTlsMode
  /** STARTTLS on an `smtp://` URL: 0 none, 1 try, 2 require. Zero for `implicit`, which is
   *  already encrypted and has nothing to upgrade. */
  readonly starttls: 0 | 1 | 2
  readonly user: string
  readonly password: string
  readonly sender: string
  readonly senderName: string
}

const STARTTLS: Readonly<Record<SmtpTlsMode, 0 | 1 | 2>> = {
  none: 0,
  starttls: 1,
  require: 2,
  implicit: 0,
}

/** The environment's answer to "where does mail go", as one value. */
export function smtpRelay(config: EmailConfig): SmtpRelay {
  const scheme = config.EMAIL_SMTP_TLS === 'implicit' ? 'smtps' : 'smtp'
  return {
    url: `${scheme}://${config.EMAIL_SMTP_HOST}:${config.EMAIL_SMTP_PORT}`,
    host: config.EMAIL_SMTP_HOST,
    port: config.EMAIL_SMTP_PORT,
    tls: config.EMAIL_SMTP_TLS,
    starttls: STARTTLS[config.EMAIL_SMTP_TLS],
    user: config.EMAIL_USERNAME,
    password: config.EMAIL_PASSWORD,
    sender: config.EMAIL_SENDER,
    senderName: config.EMAIL_SENDER_NAME,
  }
}
