/// <reference types="node" />

/** One rendered mail: the subject, the HTML document, and the plain text alternative. */
export interface RenderedMail {
  subject: string
  html: string
  /** Produced from the same document at build time — what a receiver without HTML sees. */
  text: string
}

export interface RenderedMailBytes {
  subject: Buffer
  html: Buffer
  text: Buffer
}

/** What a template accepts, read off the pug sources at build time. */
export interface TemplateInfo {
  /** e.g. "accountActivation" */
  name: string
  /** Value names, without the "locale" the renderer fills in itself. */
  slots: readonly string[]
  /** Boolean switches that select a branch, e.g. "typoCorrection". */
  flags: readonly string[]
  /** How many pre-rendered branch variants this template has. */
  variants: number
}

export interface Limits {
  maxStaticHtml: number
  maxStaticSubject: number
  maxStaticText: number
  maxSlotRefs: number
  /** What the addon's own arena was sized to. */
  arenaBytes: number
}

/** An inline attachment referenced from the templates as `cid:<cid>`. */
export interface Asset {
  cid: string
  filename: string
  contentType: string
  size: number
  data: Buffer
}

export interface MailerConfig {
  /** smtp://host:port for plain or STARTTLS, smtps://host:port for implicit TLS. */
  url: string
  from: string
  fromName?: string
  user?: string
  pass?: string
  /** The one CA certificate the relay is verified against. */
  cainfo?: string
  /** 0 none, 1 try, 2 require. Ignored for smtps://. */
  starttls?: number
  /** Development only. */
  insecure?: boolean
  /** Round trip one send is given. Default 10000. */
  timeoutMs?: number
  /**
   * How many sends may be on the libuv thread pool at once. Default `UV_THREADPOOL_SIZE - 1`,
   * so one thread stays free for the `fs`, `dns` and `crypto` work that shares that pool.
   */
  maxConcurrent?: number
}

export interface MailerStats {
  sent: number
  failed: number
  /** Sends on the libuv thread pool and not settled yet — never more than `limit`. */
  pending: number
  /** Sends waiting for a slot, because `limit` are already in flight. */
  waiting: number
  /** What `maxConcurrent` resolved to. */
  limit: number
}

export interface Mail {
  to: string
  subject: string
  /** The plain text alternative. `body` is the older name for the same thing. */
  text?: string
  body?: string
  /** The HTML alternative. Both together make a multipart/alternative message. */
  html?: string
}

/** Renders into JS strings. The 21 KB of UTF-8 costs more than the render. */
export function render(
  template: string,
  locale: string,
  values: Record<string, string | boolean | null | undefined>,
): RenderedMail

/** The same, as Buffers -- a copy rather than a UTF-8 conversion, so ~2x cheaper. */
export function renderBytes(
  template: string,
  locale: string,
  values: Record<string, string | boolean | null | undefined>,
): RenderedMailBytes

/** Every template in the binary, by name. */
export const templates: ReadonlyMap<string, TemplateInfo>
export const locales: readonly string[]
export const limits: Limits
export function assets(): Asset[]

export class Mailer {
  constructor(config: MailerConfig)
  /**
   * Renders and sends, without the document ever becoming a JS value. One connection per
   * mail, on a libuv thread pool thread; the promise resolves with the Message-ID once the
   * relay has taken it, and rejects with the relay's own words when it has not.
   *
   * The pool has four threads by default (UV_THREADPOOL_SIZE) and shares them with fs, dns
   * and crypto -- so four sends may be in flight. Bulk mail belongs in fast-servers.
   */
  send(
    to: string,
    template: string,
    locale: string,
    values: Record<string, string | boolean | null | undefined>,
  ): Promise<string>
  /**
   * A message the caller rendered itself: text, HTML, or both as a multipart/alternative.
   * Inline images are not attached to this one — `send()` is what carries the templates'
   * six `cid:` images.
   */
  sendMail(mail: Mail): Promise<string>
  readonly stats: MailerStats
  /** Refuses further sends. Mails already out settle their promises first. */
  close(): void
}
