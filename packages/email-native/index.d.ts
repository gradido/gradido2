/// <reference types="node" />

/** One rendered mail. `subject` is plain text, `html` is the document. */
export interface RenderedMail {
  subject: string
  html: string
}

export interface RenderedMailBytes {
  subject: Buffer
  html: Buffer
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
  /** 0 sends on the calling thread via drain(); 1 or more starts workers. */
  workers?: number
  /**
   * Ceiling on worker threads. Leave it set: 0 makes the C side ask libuv how
   * many cores the machine has, which Bun's N-API shim cannot answer and which
   * aborts the process there (oven-sh/bun#18546).
   */
  workerMax?: number
  queueMax?: number
  messageMax?: number
  timeoutMs?: number
}

export interface MailerStats {
  queued: number
  sent: number
  retried: number
  failed: number
  pending: number
  workers: number
}

export interface Mail {
  to: string
  subject: string
  /** Sent as text/plain; see the README on what the mailer cannot do yet. */
  body: string
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
  /** Renders and queues without the document ever becoming a JS value. */
  send(
    to: string,
    template: string,
    locale: string,
    values: Record<string, string | boolean | null | undefined>,
  ): void
  enqueue(mail: Mail): void
  readonly stats: MailerStats
  /** Blocks. Shutdown and tests only. */
  drain(timeoutMs?: number): boolean
  close(): void
}
