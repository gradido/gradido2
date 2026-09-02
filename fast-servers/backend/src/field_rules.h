/*
 * The field rules of `packages/shared/src/schemas`, in C.
 *
 * A route owns what is HTTP -- which body is accepted, which status is answered -- and the rules
 * below are the "which body". On the TypeScript path they are valibot schemas shared with the
 * registration form, so the page and the route cannot disagree about what a first name is; here
 * they are three functions, and what keeps the two honest is that a request refused by one is
 * refused by the other, with the same message.
 *
 * **The measure is a JavaScript string's, not a byte count**, and that is not pedantry: valibot's
 * maxLength and minLength count UTF-16 code units, and a German first name is full of characters
 * that are one unit and two bytes. Counting bytes would refuse names the reference path accepts
 * and, worse, would size a buffer against the wrong number. The same goes for trimming, where
 * JavaScript removes rather more than the ASCII whitespace C would think of.
 */
#ifndef BACKEND_FIELD_RULES_H
#define BACKEND_FIELD_RULES_H

#include <stddef.h>

/**
 * Characters in @p text as JavaScript counts them: UTF-16 code units over @p length bytes of
 * UTF-8. A malformed byte counts as one unit, so a broken encoding is a length rather than a
 * crash -- what refuses it is the rule that wanted a name, further down.
 */
size_t bk_utf16_length(const char *text, size_t length);

/**
 * The substring `String.prototype.trim` would leave, as an offset and a length into @p text.
 *
 * The set is ECMAScript's WhiteSpace and LineTerminator, which is wider than C's isspace: U+00A0
 * and U+FEFF are in it, and a value pasted out of a web page carries them.
 */
void bk_trim(const char *text, size_t length, size_t *begin, size_t *trimmed_length);

/**
 * Whether @p text is an email address by the rule `v.email()` applies -- valibot 1.4.2's
 * EMAIL_REGEX, which is
 *
 *     /^[\w+-]+(?:\.[\w+-]+)*@[\da-z]+(?:[.-][\da-z]+)*\.[a-z]{2,}$/iu
 *
 * and therefore ASCII-only: `\w` under the `u` flag is `[A-Za-z0-9_]` and nothing wider. It is
 * written out here rather than pulled apart into a regex engine, and the one place it needs care
 * is the domain: the trailing `\.[a-z]{2,}` is what the repetition before it has to leave room
 * for, so the last dot is found first and the labels before it are checked separately.
 *
 * That the rule is narrower than RFC 5321 is deliberate on the reference path and is copied
 * rather than improved on: an address one implementation accepts and the other does not is a
 * member who can register against one deployment and not the next.
 */
int bk_is_email(const char *text, size_t length);

#endif /* BACKEND_FIELD_RULES_H */
