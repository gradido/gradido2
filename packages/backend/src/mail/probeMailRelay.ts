import { Mailer } from '@gradido/email-native'
import type { SmtpRelay } from '@gradido/service-core'

/**
 * Asks the relay whether it would take a mail, without sending one.
 *
 * Answers `undefined` when it would, and the relay's own words when it would not. Never
 * throws: whether an unreachable relay is worth a warning or the end of a startup is the
 * caller's decision, and `main.ts` has decided it is a warning.
 *
 * **It goes through the mailer rather than speaking SMTP here**, and that is the whole point
 * of the file. The session is opened by the same libcurl, over the same TLS stack, with the
 * same trust configuration the sends will use — so a relay this accepts is a relay the sends
 * can reach. A probe written against Node's `tls` would answer a different question: a
 * certificate Node trusts and mbedtls does not would pass it and fail every mail after it.
 *
 * The mailer is built here and closed again because nothing in this process sends yet. When
 * the first mail is sent it belongs on the `AppContext`, held open for the life of the
 * process, and this becomes `context.mailer.verify()`.
 */
export async function probeMailRelay(relay: SmtpRelay): Promise<string | undefined> {
  const mailer = new Mailer({
    url: relay.url,
    from: relay.sender,
    fromName: relay.senderName === '' ? undefined : relay.senderName,
    /* Both or neither: SMTP AUTH is a pair, and half of one authenticates as nobody. */
    user: relay.user === '' ? undefined : relay.user,
    pass: relay.user === '' ? undefined : relay.password,
    starttls: relay.starttls,
    /* Short on purpose: this runs while a server is starting, and a relay that has not greeted
       in five seconds is one to warn about rather than one to keep waiting for. */
    timeoutMs: 5000,
  })
  try {
    await mailer.verify()
    return undefined
  } catch (error) {
    return error instanceof Error ? error.message : String(error)
  } finally {
    mailer.close()
  }
}
