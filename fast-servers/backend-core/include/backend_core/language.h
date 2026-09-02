/*
 * The languages a member can be written to in -- contracts/types/Language.json.
 *
 * This is not the set of locales a frontend has catalogs for. That set is smaller, it changes
 * when someone finishes a translation, and it concerns one implementation only; see
 * contracts/AGENTS.md, working rule 3. A member may well be addressed in a language the
 * interface does not speak yet.
 */
#ifndef BACKEND_CORE_LANGUAGE_H
#define BACKEND_CORE_LANGUAGE_H

/** `users.language` is varchar(4); every value is two characters and this is the column. */
#define BC_LANGUAGE_MAX 5

/** What an unknown or absent language becomes. */
#define BC_LANGUAGE_DEFAULT "de"

/** Non-zero when @p value is one of the contracted languages. */
int bc_language_is_known(const char *value);

/**
 * @p value, or the default when it is absent or not a language this contract knows.
 *
 * That is `unknownValuePolicy: "ignore_and_warn"` in the contract, and it is deliberate: the
 * value arrives from the browser's locale, which nobody typed and nobody can correct from the
 * form. A registration that fails because a visitor's browser says `de-AT` would be a bug in us,
 * not in them. Never NULL.
 */
const char *bc_language_or_default(const char *value);

#endif /* BACKEND_CORE_LANGUAGE_H */
