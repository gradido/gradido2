import type { UserCreateRequest } from '@gradido/shared/schemas'
import type { BackendContext } from '../../../BackendContext'
import { newGradidoId } from '../logic/gradidoId.logic'
import { newEmailVerificationCode } from '../logic/verificationCode.logic'
import { UserRepository } from '../repositories'

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
/**
 * How many times a registration is attempted before the draws are treated as broken.
 *
 * Reaching this is not bad luck: a v4 gradido_id is 122 random bits and the verification code
 * is 53, so five collisions in a row is not a number that happens. It is the CSPRNG answering
 * the same thing every time, and a loop that kept going would hide that forever.
 */
const MAX_ATTEMPTS = 5

export async function registerAccount(
  context: BackendContext,
  request: UserCreateRequest,
): Promise<void> {
  const users = new UserRepository(context.db)

  for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    const result = await users.createAccount({
      /* Already trimmed and lowercased: `emailSchema` does it in the pipe, so one address is
         one string before it ever reaches here. That matters more than it used to --
         `user_contacts_email_key` compares the bytes it is given, and it is now the only thing
         deciding whether an address is taken. */
      email: request.email,
      firstName: request.firstName,
      lastName: request.lastName,
      language: request.language,
      /* The community this instance is. It is on the context rather than looked up here:
         one row, written once at setup, and the process refuses to start without it. */
      communityId: context.homeCommunity.id,
      /* Drawn, not checked. Both of these are answered by an index at the moment of the write,
         and a collision comes back below as something to draw again. */
      gradidoId: newGradidoId(),
      emailVerificationCode: newEmailVerificationCode(),
      createdAt: new Date(),
    })

    if ('collided' in result) {
      /* Two random values can land on one that exists, and they are not equally unlikely: a v4
         gradido_id is 122 bits and will not happen, while the verification code is bounded to
         2^53-1 for SQLite's sake, where a community of a million members has a birthday chance
         of roughly one in eighteen thousand over its whole life. Both are drawn again on the
         next turn of this loop.
         Logged at warn rather than passed over, because a run of these is not luck: it is a
         generator that has stopped being random, and the only way anybody finds out is if the
         rare case says something when it happens. */
      context.logger.warn(
        {
          cat: 'user',
          event: 'user.registration.collision',
          data: { constraint: result.collided, attempt },
        },
        'a generated value was already taken, drawing again',
      )
      continue
    }

    if ('takenBy' in result) {
      context.logger
        .child({ usr: result.takenBy })
        .info(
          { cat: 'user', event: 'user.registration.denied', data: { reason: 'address-in-use' } },
          'registration for an address that is already in use, answering as if it were new',
        )

      // TODO: legacy mails the member who *owns* the address — in their language and with
      // their name, never the new registrant's — so that somebody typing the wrong address
      // is noticed by the person who would otherwise never hear about it. It needs their name
      // and language, which this no longer reads: the id is what the log line is contracted to
      // carry, and loading a whole member for a mail nobody sends yet would be a round trip
      // spent on a comment.
      // await sendAccountMultiRegistrationEmail({ ...owner, email })
      // await EVENT_EMAIL_ACCOUNT_MULTIREGISTRATION(owner)

      return
    }

    context.logger
      .child({ usr: result.created })
      .info(
        { cat: 'user', event: 'user.registration.created', data: { language: request.language } },
        'account created',
      )
    return
  }

  /* Every draw landed on a value that exists. At these widths that is not a coincidence
     happening five times, it is a generator that has stopped generating -- so this is an error
     and not another turn of the loop, and the caller answers 500 rather than a silent 204 that
     would tell somebody their registration went through. */
  throw new Error(`no free generated values after ${MAX_ATTEMPTS} attempts`)

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
