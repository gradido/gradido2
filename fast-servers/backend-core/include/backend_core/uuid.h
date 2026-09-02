/*
 * A v4 uuid, in the 36 character form the columns hold.
 *
 * Two of them are drawn on the paths in this component -- `communities.community_uuid` and
 * `users.gradido_id` -- and both are public identifiers rather than secrets. They come out of
 * the system CSPRNG all the same: a predictable identifier is a way to enumerate rows, and
 * libsodium's randombytes is already in this process.
 */
#ifndef BACKEND_CORE_UUID_H
#define BACKEND_CORE_UUID_H

/** A uuid in its 36 character form, and the terminator. */
#define BC_UUID_TEXT_MAX 37

/** Fills @p out with a fresh v4 uuid, lowercase and hyphenated, NUL terminated. */
void bc_new_uuid(char *out);

#endif /* BACKEND_CORE_UUID_H */
