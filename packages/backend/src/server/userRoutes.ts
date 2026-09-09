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
      // return always ok, because we don't want attackers to scan or user emails and getting something out of that
      return status(204)
    },
    {
      body: userCreateRequestSchema,
      response: {
        204: v.undefined(),
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
