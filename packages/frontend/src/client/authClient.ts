import type { LoginInput } from 'shared'
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

const isLoginErrorCode = (value: unknown): value is LoginErrorCode =>
  Object.values(LoginErrorCode).includes(value as LoginErrorCode)

export async function login(input: LoginInput): Promise<LoginResult> {
  let response: Response
  try {
    response = await fetch(`${CONFIG.API_BASE_URL}${LOGIN_PATH}`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      credentials: 'include',
      body: JSON.stringify(input),
    })
  } catch (error) {
    throw new LoginError(LoginErrorCode.Unknown, (error as Error).message)
  }

  const body = await response.json().catch(() => undefined)

  if (!response.ok) {
    const code = isLoginErrorCode(body?.error?.code) ? body.error.code : LoginErrorCode.Unknown
    throw new LoginError(code, body?.error?.message ?? `HTTP ${response.status}`)
  }

  return body as LoginResult
}
