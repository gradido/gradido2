import type { LoginInput, RegisterInput } from 'shared'
import { CONFIG } from '../config'

/**
 * Reasons a login can fail that the page reacts to differently. Anything else is
 * reported as an unknown error rather than guessed at.
 */
export enum LoginErrorCode {
  EmailNotValidated = 'user.email-not-validated',
  NoPasswordSet = 'user.no-password-set',
  InvalidCredentials = 'user.invalid-credentials',
  Unknown = 'unknown',
}

export class LoginError extends Error {
  constructor(
    readonly code: LoginErrorCode,
    message: string,
  ) {
    super(message)
    this.name = 'LoginError'
  }
}

export interface LoginResult {
  gradidoId: string
  firstName: string
  lastName: string
  language: string
}

// TODO: the backend has no auth route yet. Once it does, its definition belongs in
// `contracts/server` and in `packages/shared`, and this call becomes an Eden Treaty
// call whose request and response types come from there instead of being declared
// here. See Architecture.md, *HTTP server*.
const LOGIN_PATH = '/auth/login'
const REGISTER_PATH = '/auth/register'

const isLoginErrorCode = (value: unknown): value is LoginErrorCode =>
  Object.values(LoginErrorCode).includes(value as LoginErrorCode)

async function post(path: string, input: unknown): Promise<unknown> {
  let response: Response
  try {
    response = await fetch(`${CONFIG.API_BASE_URL}${path}`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      credentials: 'include',
      body: JSON.stringify(input),
    })
  } catch (error) {
    throw new LoginError(LoginErrorCode.Unknown, (error as Error).message)
  }

  const body = (await response.json().catch(() => undefined)) as
    | { error?: { code?: unknown; message?: string } }
    | undefined

  if (!response.ok) {
    const code = isLoginErrorCode(body?.error?.code) ? body.error.code : LoginErrorCode.Unknown
    throw new LoginError(code, body?.error?.message ?? `HTTP ${response.status}`)
  }
  return body
}

/**
 * Register. Nothing comes back but success: the account is not usable until the address
 * is confirmed, so the page's job afterwards is to send the visitor to their inbox.
 */
export async function register(input: RegisterInput & { language: string }): Promise<void> {
  await post(REGISTER_PATH, input)
}

export async function login(input: LoginInput): Promise<LoginResult> {
  return (await post(LOGIN_PATH, input)) as LoginResult
}
