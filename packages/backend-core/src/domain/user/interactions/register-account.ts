import type { UserCreateRequest } from '@gradido/shared/schemas'
import type { BackendContext } from '../../../BackendContext'
import { newGradidoId } from '../logic/gradidoId.logic'
import { newEmailVerificationCode } from '../logic/verificationCode.logic'
import { UserRepository } from '../repositories'
import { normalizeEmail } from '../user.data'

/**
 * Somebody signs up.
 *
 * The behavioral reference is legacy's `createUser` resolver together with the
 * `registerAccount` interaction it delegates to
 * (`../gradido/backend/src/interactions/registerAccount/RegisterAccount.context.ts`). This
 * is the first slice of it: **two rows and nothing else**. Everything legacy does around
 * those rows is listed at the bottom of this file, in the order it happens there, so that
 * what is missing is a list to work through rather than something to rediscover.
 *
 * Two properties of the legacy flow are *not* deferred, because they are the ones that stop
 * being addable later:
 *
 * **The silence rule.** A registration for an address that already exists answers exactly
 * like one for an address that does not — an empty 204, no delay worth measuring, and no row
 * written. `user_contacts.email` is globally unique, so the alternative is not a neutral
 * answer but a constraint violation, and a 500 that only ever happens for registered
 * addresses is a membership oracle for anyone with a list of email addresses. Legacy fakes a
 * user object to say nothing with; the route here says nothing by having nothing to say,
 * which is the same property without anything to keep true.
 *
 * **One instant, one transaction.** Both rows carry the same `created_at` and are written
 * together or not at all — see `UserRepository.createAccount`.
 *
 * Nothing is cached: an account that did not exist a moment ago is in no session, and the
 * member cannot sign in until the address is confirmed. There is no invalidation to make
 * visible here, which is why this Interaction says nothing about it.
 */
export async function registerAccount(
  context: BackendContext,
  request: UserCreateRequest,
): Promise<void> {
  const email = normalizeEmail(request.email)
  const users = new UserRepository(context.db)

  const owner = await users.findAddressOwner(email)
  if (owner !== undefined) {
    context.logger
      .child({ usr: owner.id })
      .info(
        { cat: 'user', event: 'user.registration.denied', data: { reason: 'address-in-use' } },
        'registration for an address that is already in use, answering as if it were new',
      )

    // TODO: legacy mails the member who *owns* the address — in their language and with
    // their name, never the new registrant's — so that somebody typing the wrong address
    // is noticed by the person who would otherwise never hear about it.
    // await sendAccountMultiRegistrationEmail({ ...owner, email })
    // await EVENT_EMAIL_ACCOUNT_MULTIREGISTRATION(owner)

    return
  }

  const userId = await users.createAccount({
    email,
    firstName: request.firstName,
    lastName: request.lastName,
    language: request.language,
    /* The community this instance is. It is on the context rather than looked up here:
       one row, written once at setup, and the process refuses to start without it. */
    communityId: context.homeCommunity.id,
    /* Drawn until it is free in this community — see gradidoId.logic.ts. */
    gradidoId: await newGradidoId((candidate) =>
      users.gradidoIdExists(candidate, context.homeCommunity.id),
    ),
    emailVerificationCode: newEmailVerificationCode(),
    createdAt: new Date(),
  })

  context.logger
    .child({ usr: userId })
    .info(
      { cat: 'user', event: 'user.registration.created', data: { language: request.language } },
      'account created',
    )

  // TODO, in the order legacy does them, each waiting on something that does not exist yet:
  //
  //   alias                   pickFreeAlias() over aliasCandidates(), written to users.alias
  //                           and to user_aliases with origin ASSIGNED. Waits on that table
  //                           and on the alias ladder in shared.
  //   redeemCode              CL- prefixed codes point at a contribution_link, everything
  //                           else at a transaction_link, and set contribution_link_id or
  //                           referrer_id. Waits on both tables.
  //   project                 project_brandings, for the logo on the activation mail.
  //   publisherId             Elopage, users.publisher_id.
  //   the activation mail     sendAccountActivationEmail with EMAIL_LINK_VERIFICATION plus
  //                           the verification code. Until this exists, an account cannot be
  //                           activated at all — this is the next thing to write.
  //   EVENT_EMAIL_CONFIRMATION, EVENT_USER_REGISTER   the events table.
  //   registerAddressTransaction                      the member's address on the blockchain.
  //   syncHumhub, sendUsersToGms                      external systems, both behind a flag.
  //
  // Of these, only the mail is on the critical path for a usable registration.
}
