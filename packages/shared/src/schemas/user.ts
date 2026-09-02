import * as v from 'valibot'
import { emailSchema, firstNameSchema, lastNameSchema } from './auth'
import { languageSchema } from './language'

/**
 * `user.create` — the body of `POST /user/create`, `contracts/server/backend/user.json`.
 *
 * It is composed of the same field schemas the registration form validates with, so the
 * page and the route cannot disagree about what a first name is. The route in
 * `@gradido/backend` uses this object as its `body`; the frontend only needs the type.
 *
 * Here rather than beside the route, because a schema is a *value* and the browser loads
 * it: everything under `packages/backend` reaches Elysia and the database. See
 * `AGENTS.md`, section 2.
 *
 * The contracted request minus the four fields no interaction reads yet. They are named
 * here rather than accepted and ignored: a field a route takes and drops is worse than one
 * it does not take, because a caller cannot tell the two apart.
 *
 * ```text
 * alias         needs the alias ladder and user_aliases  (legacy: pickFreeAlias)
 * publisherId   Elopage, needs login_elopage_buys
 * redeemCode    needs contribution_links / transaction_links
 * project       needs project_brandings
 * ```
 */
export const userCreateRequestSchema = v.object({
  firstName: firstNameSchema,
  lastName: lastNameSchema,
  email: emailSchema,
  language: languageSchema,
})

export type UserCreateRequestInput = v.InferInput<typeof userCreateRequestSchema>
export type UserCreateRequest = v.InferOutput<typeof userCreateRequestSchema>
