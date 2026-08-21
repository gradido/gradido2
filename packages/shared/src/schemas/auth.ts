import * as v from 'valibot'

// Issue messages are English source strings on purpose: the frontend feeds them
// through gettext, the backend logs them as-is. See `translateIssue` in frontend-core.

/** contracts/db/user_contacts.json — `email varchar(255)`. */
export const EMAIL_MAX_LENGTH = 255

/**
 * Prevalidation: what is wrong the moment it is typed, whatever the user types next.
 *
 * Nothing here can be fixed by continuing to type, which is what makes it safe to report
 * immediately. A half-written address is not in this set — it is unfinished, not wrong.
 *
 * Deliberately does *not* trim. A trailing space has to be reported as the space is
 * typed; trimming first would hide it until the next keystroke turned it into an
 * interior one. An address pasted with surrounding whitespace still passes, because
 * `FormField` reports a valid value before it consults prevalidation at all.
 */
export const emailPrevalidateSchema = v.pipe(
  v.string(),
  v.maxLength(EMAIL_MAX_LENGTH, 'This email address is too long'),
  // Any whitespace, not just U+0020 — a paste can carry a tab or a newline.
  v.regex(/^\S*$/u, 'An email address cannot contain spaces'),
)

/** The full rule. Trims first, so an address pasted with padding keeps working. */
export const emailSchema = v.pipe(
  v.string(),
  v.trim(),
  v.maxLength(EMAIL_MAX_LENGTH, 'This email address is too long'),
  v.nonEmpty('This field is required'),
  v.email('Please enter a valid email address'),
)

/** contracts/db/users.json — `first_name` and `last_name` are `varchar(255)`. */
export const NAME_MAX_LENGTH = 255

// Legacy asks for three characters of a first name and two of a last name. Both are
// deliberately lenient: names are shorter and stranger than form designers expect.
const MIN_FIRST_NAME = 3
const MIN_LAST_NAME = 2

/** Wrong however the name continues: it cannot outgrow the column that stores it. */
const namePrevalidate = v.pipe(v.string(), v.maxLength(NAME_MAX_LENGTH, 'This name is too long'))

export const firstNamePrevalidateSchema = namePrevalidate
export const lastNamePrevalidateSchema = namePrevalidate

export const firstNameSchema = v.pipe(
  v.string(),
  v.trim(),
  v.maxLength(NAME_MAX_LENGTH, 'This name is too long'),
  v.nonEmpty('This field is required'),
  v.minLength(MIN_FIRST_NAME, 'Please enter at least three characters'),
)

export const lastNameSchema = v.pipe(
  v.string(),
  v.trim(),
  v.maxLength(NAME_MAX_LENGTH, 'This name is too long'),
  v.nonEmpty('This field is required'),
  v.minLength(MIN_LAST_NAME, 'Please enter at least two characters'),
)

/** Consent is a decision, so there is nothing to prevalidate — it is given or it is not. */
export const privacyConsentSchema = v.pipe(
  v.boolean(),
  v.check((agreed) => agreed, 'Please agree to the privacy policy'),
)

// A password has no prevalidation: no keystroke makes it wrong, only unfinished. Login
// only checks that one was entered — the strength rules belong to registration and
// password reset, which validate against the stored policy.
export const loginPasswordSchema = v.pipe(v.string(), v.nonEmpty('This field is required'))

export const loginSchema = v.object({
  email: emailSchema,
  password: loginPasswordSchema,
})

export type LoginInput = v.InferOutput<typeof loginSchema>

export const registerSchema = v.object({
  firstName: firstNameSchema,
  lastName: lastNameSchema,
  email: emailSchema,
})

export type RegisterInput = v.InferOutput<typeof registerSchema>
