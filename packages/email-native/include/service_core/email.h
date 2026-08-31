/* The e-mail renderer's API. It does not change when a template changes --
 * everything template-dependent is generated into service_core/email_gen.h out of
 * the pug sources.
 *
 * A rendered document is a sequence of ops: either a slice of the static byte
 * pool (memcpy) or a slot (user data, HTML-escaped). No parser, no per-op
 * allocation, nothing to decompress.
 *
 * The file belongs to packages/email-native. The copy under
 * fast-servers/service-core/include is written by that package's build
 * (scripts/sync-fast-servers.ts) and is overwritten by the next one, so a change
 * has to be made in packages/email-native/include/service_core/email.h.
 */
#ifndef SERVICE_CORE_EMAIL_H
#define SERVICE_CORE_EMAIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char  *data; /* NUL-terminated; release with ge_str_free */
    size_t len;
} ge_str_t;

typedef struct {
    ge_str_t subject;
    ge_str_t html;
} ge_mail_t;

/* len == GE_SLOT_* means: 'off' is a slot index, not a pool offset. */
#define GE_SLOT_HTML 0xFFFFFFFFu /* value is inserted HTML-escaped */
#define GE_SLOT_RAW  0xFFFFFFFEu /* value is inserted verbatim (subject line) */

typedef struct {
    uint32_t off;
    uint32_t len;
} ge_op_t;

typedef struct {
    uint32_t start;
    uint32_t count;
} ge_prog_t;

typedef struct {
    const char          *cid;
    const char          *filename;
    const char          *content_type;
    const unsigned char *data;
    size_t               size;
} ge_asset_t;

/* "set" means: not NULL and not empty -- this is how the generated wrappers
 * decide the if-branches that came from the pug templates. */
#define GE_HAS(s) ((s) != NULL && (s)[0] != '\0')

/* Runs a program. Two passes: length first, then exactly one malloc.
 * Returns 0 on success, -1 on out of memory. */
int  ge_run(const char *pool, const ge_op_t *ops, uint32_t count, const char *const *slots,
            ge_str_t *out);

void ge_str_free(ge_str_t *s);
void ge_mail_free(ge_mail_t *m);

/* --------------------------------------------------------------------------
 * Arena: one buffer that stands for the whole run and is merely reset per mail.
 * No malloc/free on the hot path. The ge_str_t point into the arena and must
 * NOT be freed -- they are valid until the next ge_arena_reset(). One arena
 * per thread.
 * -------------------------------------------------------------------------- */
typedef struct {
    char  *base;
    size_t cap;
    size_t used;
} ge_arena_t;

int  ge_arena_init(ge_arena_t *a, size_t cap);
void ge_arena_free(ge_arena_t *a);

/* Grows the arena to at least 'need' bytes. Discards the contents, so it may
 * only run when no ge_str_t is still in use -- meant for the case where
 * ge_run_into returns -1: grow, then render again. After that the arena is at
 * its final size and never allocates again. */
int  ge_arena_ensure(ge_arena_t *a, size_t need);

static inline void ge_arena_reset(ge_arena_t *a) { a->used = 0; }

/* Two passes, but checked: -1 if there is not enough room (out->len then holds
 * the requirement). */
int ge_run_into(const char *pool, const ge_op_t *ops, uint32_t count, const char *const *slots,
                ge_arena_t *a, ge_str_t *out);

/* One pass, unchecked. The caller guarantees the room -- via
 * GE_BUF_SIZE(max_field) plus a length check on the inputs. Otherwise it writes
 * past the end of the buffer. */
int ge_run_into_fast(const char *pool, const ge_op_t *ops, uint32_t count,
                     const char *const *slots, ge_arena_t *a, ge_str_t *out);

#endif /* SERVICE_CORE_EMAIL_H */
