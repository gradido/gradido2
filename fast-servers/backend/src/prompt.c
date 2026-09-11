/*
 * The setup command's questions. prompt.h is the specification and
 * packages/backend/src/setup/prompt.ts is the reference this is held to.
 */
#include "prompt.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define bk_isatty(stream) _isatty(_fileno(stream))
#else
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#define bk_isatty(stream) isatty(fileno(stream))
#endif

/** What a keystroke means here. Everything else is a character or is ignored. */
enum {
    KEY_EOF = -1,
    KEY_ENTER = -2,
    KEY_UP = -3,
    KEY_DOWN = -4,
    KEY_BACKSPACE = -5,
    KEY_INTERRUPT = -6,
    KEY_IGNORED = -7
};

int bk_prompt_is_terminal(void)
{
    return bk_isatty(stdin) && bk_isatty(stdout);
}

void bk_say(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    (void)vfprintf(stdout, fmt, args);
    va_end(args);
    (void)fputc('\n', stdout);
    (void)fflush(stdout);
}

#if defined(_WIN32)

/* No raw mode here: the console's own API is a different thing from termios, and a cursor
 * nobody on this platform runs a server on is not worth a second implementation. Everything
 * below falls back to the typed form, which is the same fallback the reference path takes
 * when it cannot switch a terminal to raw mode. */
static int raw_begin(void)
{
    return 0;
}
static void raw_end(void)
{
}
static int read_key(void)
{
    return KEY_EOF;
}

/* There is no terminal state of ours to put back, so ending is all there is to do. */
static void on_interrupt(int signal_number)
{
    (void)signal_number;
    _exit(130);
}

#else

static struct termios g_saved;

/** Whether the terminal is in raw mode now -- what on_interrupt reads to know that it has
 *  something to put back. */
static volatile sig_atomic_t g_raw;

/**
 * Turns off echo and line buffering, so that a keystroke arrives as it is pressed -- and turns
 * off the signal keys, so that Ctrl-C arrives as a keystroke too, byte 3, and is answered where
 * the terminal is being read rather than in a handler. That is what raw mode means to Node's
 * setRawMode, which the reference path uses; without ISIG off, Ctrl-C would still be a SIGINT,
 * and the handler the process has for it while serving only raises a flag nobody here reads.
 */
static int raw_begin(void)
{
    struct termios raw;

    if (tcgetattr(fileno(stdin), &g_saved) != 0)
        return 0;
    raw = g_saved;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fileno(stdin), TCSANOW, &raw) != 0)
        return 0;
    g_raw = 1;
    return 1;
}

static void raw_end(void)
{
    g_raw = 0;
    (void)tcsetattr(fileno(stdin), TCSANOW, &g_saved);
}

/**
 * SIGINT while the conversation is held -- see bk_prompt_begin().
 *
 * Only what may be done in a handler: tcsetattr, write and _exit are all async-signal-safe.
 * Raw mode is put back for the case where a signal arrives anyway -- somebody sending one with
 * kill rather than with the keyboard -- because a shell left without echo is worse than any
 * setup that did not finish.
 */
static void on_interrupt(int signal_number)
{
    ssize_t written;

    (void)signal_number;
    if (g_raw)
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
    written = write(STDOUT_FILENO, "\n", 1);
    (void)written;
    _exit(130);
}

/** Whether another byte of the same escape sequence is already there. */
static int more_within(long micros)
{
    fd_set set;
    struct timeval wait;

    FD_ZERO(&set);
    FD_SET(fileno(stdin), &set);
    wait.tv_sec = 0;
    wait.tv_usec = micros;
    return select(fileno(stdin) + 1, &set, NULL, NULL, &wait) > 0;
}

static int read_key(void)
{
    unsigned char c;

    if (read(fileno(stdin), &c, 1) != 1)
        return KEY_EOF;
    if (c == 3)
        return KEY_INTERRUPT;
    if (c == '\r' || c == '\n')
        return KEY_ENTER;
    if (c == 127 || c == 8)
        return KEY_BACKSPACE;
    if (c != 27)
        return (int)c;

    /* An escape, which is either an arrow key or somebody pressing Escape. The rest of the
     * sequence is already in the buffer when it is a key; waiting a moment for it is what
     * tells the two apart without a timer of our own. */
    if (!more_within(50000) || read(fileno(stdin), &c, 1) != 1 || c != '[')
        return KEY_IGNORED;
    if (!more_within(50000) || read(fileno(stdin), &c, 1) != 1)
        return KEY_IGNORED;
    if (c == 'A' || c == 'D')
        return KEY_UP;
    if (c == 'B' || c == 'C')
        return KEY_DOWN;
    return KEY_IGNORED;
}

#endif

/** What SIGINT did before bk_prompt_begin(), for bk_prompt_end() to put back. */
static void (*g_previous_interrupt)(int) = SIG_DFL;

void bk_prompt_begin(void)
{
    void (*previous)(int) = signal(SIGINT, on_interrupt);

    if (previous != SIG_ERR)
        g_previous_interrupt = previous;
}

void bk_prompt_end(void)
{
    (void)signal(SIGINT, g_previous_interrupt);
}

static void render(const char *const *labels, const char *const *hints, int count, int index)
{
    int i;

    for (i = 0; i != count; ++i) {
        const char *hint = hints != NULL && hints[i] != NULL ? hints[i] : NULL;

        /* Cleared line by line rather than as a block: the line before may have been longer,
         * and half a hint left standing under a shorter one reads as part of it. */
        (void)fprintf(stdout, "\033[2K%s%s%s%s%s%s\n", i == index ? "\xe2\x9d\xaf " : "  ",
                      i == index ? "\033[1m" : "", labels[i], i == index ? "\033[22m" : "",
                      hint != NULL ? "  \xe2\x80\x94 " : "", hint != NULL ? hint : "");
    }
    (void)fflush(stdout);
}

/** The list as something to type a number into, for a terminal that has no raw mode. */
static int choose_numbered(const char *const *labels, const char *const *hints, int count,
                           int initial)
{
    char line[BK_ANSWER_MAX];
    int i;

    for (i = 0; i != count; ++i) {
        const char *hint = hints != NULL && hints[i] != NULL ? hints[i] : NULL;

        (void)fprintf(stdout, "  %d) %s%s%s\n", i + 1, labels[i],
                      hint != NULL ? " \xe2\x80\x94 " : "", hint != NULL ? hint : "");
    }
    for (;;) {
        (void)fprintf(stdout, "Choose 1-%d (%d): ", count, initial + 1);
        (void)fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL)
            return -1;
        for (i = 0; line[i] != '\0'; ++i) {
            if (line[i] == '\n' || line[i] == '\r') {
                line[i] = '\0';
                break;
            }
        }
        if (line[0] == '\0')
            return initial;
        for (i = 0; i != count; ++i) {
            if ((line[0] == (char)('1' + i) && line[1] == '\0') || strcmp(line, labels[i]) == 0)
                return i;
        }
        (void)fprintf(stderr, "  Please answer with a number between 1 and %d\n", count);
    }
}

int bk_prompt_choose(const char *question, const char *const *labels, const char *const *hints,
                     int count, int initial)
{
    int index = initial;

    if (labels == NULL || count <= 0)
        return -1;
    if (initial < 0 || initial >= count)
        index = 0;

    (void)fprintf(stdout, "\n%s\n", question);
    (void)fflush(stdout);
    if (!bk_prompt_is_terminal() || !raw_begin())
        return choose_numbered(labels, hints, count, index);

    render(labels, hints, count, index);
    for (;;) {
        int key = read_key();

        if (key == KEY_EOF) {
            raw_end();
            return -1;
        }
        if (key == KEY_INTERRUPT) {
            /* Ctrl-C, which raw mode does not turn into a signal -- see raw_begin. 130 is what a
             * shell reports for a program that ended on SIGINT, and setup has written nothing at
             * this point. */
            raw_end();
            (void)fputc('\n', stdout);
            exit(130);
        }
        if (key == KEY_ENTER)
            break;
        if (key >= '1' && key < '1' + count) {
            index = key - '1';
            break;
        }
        if (key == KEY_UP || key == KEY_DOWN) {
            index = (index + (key == KEY_UP ? count - 1 : 1)) % count;
            (void)fprintf(stdout, "\033[%dA", count);
            render(labels, hints, count, index);
        }
    }

    raw_end();
    /* The list collapses to the answer, so that a finished setup reads back as questions and
     * answers rather than as a screenful of menus. */
    (void)fprintf(stdout, "\033[%dA\033[J\xe2\x9d\xaf %s\n", count, labels[index]);
    (void)fflush(stdout);
    return index;
}

int bk_prompt_yes_no(const char *question, int initial)
{
    static const char *const kLabels[] = {"yes", "no"};
    int chosen = bk_prompt_choose(question, kLabels, NULL, 2, initial ? 0 : 1);

    return chosen < 0 ? -1 : (chosen == 0);
}

/** Copies @p value into @p out, or answers 0 when it does not fit. */
static int fits(const char *value, char *out, size_t out_size)
{
    size_t len = strlen(value);

    if (len + 1 > out_size)
        return 0;
    memcpy(out, value, len + 1);
    return 1;
}

int bk_prompt_text(const char *label, const char *fallback, char *out, size_t out_size)
{
    char line[BK_ANSWER_MAX];

    if (fallback == NULL)
        fallback = "";
    for (;;) {
        size_t length;
        size_t begin = 0;

        if (fallback[0] == '\0')
            (void)fprintf(stdout, "%s: ", label);
        else
            (void)fprintf(stdout, "%s (%s): ", label, fallback);
        (void)fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL)
            return 0;

        length = strlen(line);
        while (length != 0 && (line[length - 1] == '\n' || line[length - 1] == '\r' ||
                               line[length - 1] == ' ' || line[length - 1] == '\t'))
            line[--length] = '\0';
        while (begin != length && (line[begin] == ' ' || line[begin] == '\t'))
            ++begin;

        if (fits(begin == length ? fallback : line + begin, out, out_size))
            return 1;
        (void)fprintf(stderr, "  This value is too long\n");
    }
}

int bk_prompt_secret(const char *label, const char *fallback, const char *shown, char *out,
                     size_t out_size)
{
    char typed[BK_ANSWER_MAX];

    if (fallback == NULL)
        fallback = "";
    if (shown == NULL)
        shown = fallback[0] == '\0' ? "none" : "unchanged";
    for (;;) {
        size_t length = 0;

        (void)fprintf(stdout, "%s (%s): ", label, shown);
        (void)fflush(stdout);

        if (!bk_prompt_is_terminal() || !raw_begin()) {
            /* Visible, and the header says why that is the right trade against not asking at
             * all. The label has already been written, so this only reads. */
            if (fgets(typed, sizeof(typed), stdin) == NULL)
                return 0;
            length = strlen(typed);
            while (length != 0 && (typed[length - 1] == '\n' || typed[length - 1] == '\r'))
                typed[--length] = '\0';
        } else {
            for (;;) {
                int key = read_key();

                if (key == KEY_EOF) {
                    raw_end();
                    return 0;
                }
                if (key == KEY_INTERRUPT) {
                    raw_end();
                    (void)fputc('\n', stdout);
                    exit(130);
                }
                if (key == KEY_ENTER)
                    break;
                if (key == KEY_BACKSPACE) {
                    if (length != 0) {
                        --length;
                        (void)fputs("\b \b", stdout);
                        (void)fflush(stdout);
                    }
                    continue;
                }
                /* Anything that is not a character -- an arrow key among them -- is nothing to
                 * do here: there is no cursor to move over a value that is not shown. */
                if (key < 0)
                    continue;
                if (length + 1 < sizeof(typed)) {
                    typed[length++] = (char)key;
                    (void)fputc('*', stdout);
                    (void)fflush(stdout);
                }
            }
            raw_end();
            (void)fputc('\n', stdout);
            (void)fflush(stdout);
            typed[length] = '\0';
        }

        if (fits(length == 0 ? fallback : typed, out, out_size))
            return 1;
        (void)fprintf(stderr, "  This value is too long\n");
    }
}
