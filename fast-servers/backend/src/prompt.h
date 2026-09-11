/*
 * The terminal half of the setup command: one question at a time, on stdin and stdout.
 *
 * Three shapes of question and nothing else -- pick one of a few, type a value, type a value
 * nobody should see. `packages/backend/src/setup/prompt.ts` is the same three on the reference
 * path and is what this is held to: the same questions, the same defaults, the same keys.
 *
 * **Every question has a default and Enter takes it.** A setup that can be finished by holding
 * Enter down is a setup somebody will actually run.
 *
 * The cursor needs raw mode, which is termios here and is not available on Windows -- there,
 * and on anything that is not a terminal, a choice is a numbered list to type a number into
 * and a secret is typed in the open. The choice is the same either way; only the typing is.
 */
#ifndef BACKEND_PROMPT_H
#define BACKEND_PROMPT_H

#include <stddef.h>

/** One line of an answer. Longer than any field the setup asks for, so that a value that is
 *  too long is refused by the rule that says so rather than by the buffer. */
#define BK_ANSWER_MAX 2048

/** Whether there is somebody at the other end to answer. */
int bk_prompt_is_terminal(void);

/**
 * From here until bk_prompt_end(), Ctrl-C ends the process: with the terminal put back, a
 * newline, and 130, which is what a shell reports for a program that ended on SIGINT.
 *
 * Around the conversation and nothing else. The process has a SIGINT handler of its own, and it
 * only raises the quit flag -- right for a server, and for the database wait that follows the
 * questions, which notices the flag. Nothing between two questions reads it, so without this a
 * Ctrl-C at a typed answer, or during the probes between two questions, would be noticed by
 * nobody. The reference path gets the same from Node, where Ctrl-C ends a process that has not
 * said otherwise. Nothing has been written by then, so ending is all there is to do.
 */
void bk_prompt_begin(void);

/** Gives SIGINT back to whatever handled it before bk_prompt_begin(). */
void bk_prompt_end(void);

/** A sentence that is part of the conversation rather than part of the log. */
void bk_say(const char *fmt, ...);

/**
 * One of @p count choices, chosen with the arrow keys and Enter.
 *
 * @p hints may be NULL, and so may any entry in it; a hint is one clause after the label
 * saying what choosing it means. Answers the chosen index, or -1 when the input ended before
 * an answer -- a terminal that went away mid-setup, which is not an answer.
 */
int bk_prompt_choose(const char *question, const char *const *labels, const char *const *hints,
                     int count, int initial);

/** Yes or no, as a choice. 1 for yes, 0 for no, -1 when the input ended. */
int bk_prompt_yes_no(const char *question, int initial);

/**
 * One typed value, into @p out.
 *
 * An empty line takes @p fallback, which is what the parentheses after the label show. A value
 * that does not fit @p out is refused and asked again, because a truncated answer is the wrong
 * answer rather than a shorter one. Answers 1 when @p out holds a value and 0 at the end of
 * input.
 */
int bk_prompt_text(const char *label, const char *fallback, char *out, size_t out_size);

/**
 * The same, for a value that must not end up in the scrollback of a shared terminal.
 *
 * Echoed as asterisks where there is raw mode to do it with, and in the open where there is
 * not -- a setup that cannot ask for a password at all is worse than one that asks for it
 * visibly. An empty line keeps @p fallback, so a rerun does not have to retype it.
 *
 * The parentheses say `none` or `unchanged` rather than the value. @p shown, when it is not
 * NULL, replaces that for a fallback that is no secret to anybody -- the password
 * docker-compose.yml starts its container with is printed in that file and in README.md, and
 * `unchanged` would claim that something is configured when it is only being proposed.
 */
int bk_prompt_secret(const char *label, const char *fallback, const char *shown, char *out,
                     size_t out_size);

#endif /* BACKEND_PROMPT_H */
