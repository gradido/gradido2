import { registerAccount } from '@gradido/backend-core'
import { errorBodySchema } from '@gradido/shared/errors'
import { userCreateRequestSchema } from '@gradido/shared/schemas'
import { Elysia, status } from 'elysia'
import * as v from 'valibot'
import type { AppContext } from '../AppContext'

/**
 * The `user` domain — `contracts/server/backend/user.json`.
 *
 * One file per domain, and the file is the whole thing: path, schema, status and the call
 * into the Interaction that does the work. There is no handler interface in between. An
 * earlier layout had one, so that the routes could live in `@gradido/shared` and the
 * frontend could name their type; it bought nothing that `import type` does not buy for
 * free, and it cost a second place to look for every route. See `AGENTS.md`, section 2.
 *
 * The route stays thin all the same. It owns what is HTTP — which body is accepted, which
 * status is answered — and nothing else. Everything a second implementation has to
 * reproduce is behind `registerAccount`, in `backend-core`, where `fast-servers` has a
 * counterpart to mirror and the tests already look.
 *
 * The body has been through the valibot schema by the time the handler runs, so nothing is
 * checked again here — `AGENTS.md`, *Valibot at the boundary*.
 */
export const userRoutes = (context: AppContext) =>
  new Elysia({ name: 'gradido.user', prefix: '/user' }).post(
    '/create',
    async ({ body }) => {
      await registerAccount(context, body)
      /* 204, with no body at all. There is nothing a caller can do with a new account: it
         does not exist to them until the address is confirmed, and the page's whole job
         afterwards is to point at an inbox. Legacy answers with a `User` here and its own
         frontend asks for `{ id }` and then discards the result — so the one field it
         selects is a faked one.

         Answering with nothing also makes the silence rule structural rather than
         maintained. When the address is already taken there is no row to describe, so an
         earlier version of this route invented a gradido id and echoed the names back; two
         paths producing an indistinguishable answer is a property somebody has to keep
         true. An empty body is the same bytes either way, and there is no fabricated
         identifier for a client to mistake for a real one. */
      return status(204)
    },
    {
      body: userCreateRequestSchema,
      /* Declared per status, so Eden Treaty gives the caller a discriminated union rather
         than an `unknown` it has to guess at. The bodies are the contracted ones. */
      response: {
        204: v.undefined(),
        400: errorBodySchema,
        500: errorBodySchema,
      },
    },
  )

/**
 * What the frontend instantiates Eden Treaty with for this domain.
 *
 * A type and only a type: `treaty<UserRoutes>(url)` compiles to a wrapper around `fetch`,
 * and the `import type` that brought this in is erased before the bundler ever sees it. A
 * client that binds one domain also carries only that domain's schemas in its type graph,
 * which is why the subtype is exported here and not only the whole app's.
 */
export type UserRoutes = ReturnType<typeof userRoutes>
