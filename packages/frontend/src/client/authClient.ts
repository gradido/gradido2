import { ErrorCode } from '@gradido/shared/errors'
import type { LoginInput, UserCreateRequest } from '@gradido/shared/schemas'
import { CONFIG } from '../config'
import { userApi } from './backendClient'

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

// TODO: `user.login` has no backend route yet. When it gets one, its definition goes into
// `packages/backend/src/server/userRoutes.ts` beside `user.create` and this call becomes
// `userApi.login.post(...)` like the registration below — at which point this hand-written
// fetch and the shape it guesses at can go.
const LOGIN_PATH = '/user/login'

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

export class RegisterError extends Error {
  constructor(
    readonly code: ErrorCode,
    message: string,
  ) {
    super(message)
    this.name = 'RegisterError'
  }
}

/**
 * `user.create`. An empty 204 comes back, and that is the design: the account cannot be used
 * until the address is confirmed, and the answer is byte-identical whether or not a row was
 * written — the server must not tell a caller that an address is already registered. So the
 * page's job afterwards is to send the visitor to their inbox, in every case.
 */
export async function register(input: UserCreateRequest): Promise<void> {
  const { error } = await userApi.create.post(input)
  if (error === null) {
    return
  }

  /* A network failure has no status and no contracted body; everything else answers with
     `{ error: { code, name, message } }` from contracts/errors/api.json. */
  const body = error.value as { error?: { code?: unknown; message?: unknown } } | undefined
  const code = body?.error?.code
  throw new RegisterError(
    typeof code === 'number' && code in ErrorCode ? (code as ErrorCode) : ErrorCode.Unknown,
    typeof body?.error?.message === 'string' ? body.error.message : String(error.value),
  )
}

export async function login(input: LoginInput): Promise<LoginResult> {
  return (await post(LOGIN_PATH, input)) as LoginResult
}
