import * as v from 'valibot'

/**
 * What a person has to say about their community, and nothing a machine can work out.
 *
 * These three are the whole of the first-run setup: the key pair, the uuid and the
 * timestamps are generated, and asking about them would only be a way to get them wrong.
 * The bounds are the columns in `contracts/db/communities.json` — a value that fits the
 * form and not the column is a failure at the end of a setup rather than during it.
 *
 * They live in `@gradido/shared` rather than in the backend because the admin frontend
 * will edit the same three fields later, and a community renamed through a route has to
 * be checked by the same rules that named it at setup.
 */

/** contracts/db/communities.json — `name varchar(40)`. */
export const COMMUNITY_NAME_MAX_LENGTH = 40
/** contracts/db/communities.json — `description varchar(255)`. */
export const COMMUNITY_DESCRIPTION_MAX_LENGTH = 255
/** contracts/db/communities.json — `url varchar(255)`. */
export const COMMUNITY_URL_MAX_LENGTH = 255

export const communityNameSchema = v.pipe(
  v.string(),
  v.trim(),
  v.nonEmpty('This field is required'),
  v.maxLength(COMMUNITY_NAME_MAX_LENGTH, 'This name is too long'),
)

/**
 * Optional, and empty means absent rather than an empty string: the column is nullable and
 * `''` in it would be a third state that every reader has to remember to handle.
 */
export const communityDescriptionSchema = v.pipe(
  v.string(),
  v.trim(),
  v.maxLength(COMMUNITY_DESCRIPTION_MAX_LENGTH, 'This description is too long'),
  v.transform((description: string): string | null => (description === '' ? null : description)),
)

/**
 * Where this community answers, as an absolute URL.
 *
 * It is what other communities reach it at, so it is not a display value: a relative path
 * or a bare hostname is not something a federation partner can call. Legacy appends
 * `/api/` to it when it writes the row; gradido2 stores what was given and lets whoever
 * calls decide the path, because the api version is part of that path and it changes.
 */
export const communityUrlSchema = v.pipe(
  v.string(),
  v.trim(),
  v.nonEmpty('This field is required'),
  v.maxLength(COMMUNITY_URL_MAX_LENGTH, 'This URL is too long'),
  v.url('Please enter a valid URL, including https://'),
)

export const homeCommunitySetupSchema = v.object({
  name: communityNameSchema,
  description: communityDescriptionSchema,
  url: communityUrlSchema,
})

export type HomeCommunitySetupInput = v.InferInput<typeof homeCommunitySetupSchema>
export type HomeCommunitySetup = v.InferOutput<typeof homeCommunitySetupSchema>
