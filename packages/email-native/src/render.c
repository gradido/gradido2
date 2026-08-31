/* The runtime half of the renderer, template-independent like its header.
 *
 * The file belongs to packages/email-native. The copy under
 * fast-servers/service-core/src/email is written by that package's build
 * (scripts/sync-fast-servers.ts) and is overwritten by the next one, so a change has
 * to be made in packages/email-native/src/render.c. */
#include "service_core/email/render.h"

#include <stdlib.h>
#include <string.h>

/* Exactly the four characters pug replaces when escaping
 * (pug-runtime/index.js). That is what makes the C output byte-for-byte the
 * output pug would have produced. */
static size_t ge_esc_len(const char *s)
{
    size_t n = 0;
    for (; *s; s++) {
        switch (*s) {
        case '&': n += 5; break; /* &amp;  */
        case '<': n += 4; break; /* &lt;   */
        case '>': n += 4; break; /* &gt;   */
        case '"': n += 6; break; /* &quot; */
        default:  n += 1; break;
        }
    }
    return n;
}

static char *ge_esc_put(char *d, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': memcpy(d, "&amp;", 5);  d += 5; break;
        case '<': memcpy(d, "&lt;", 4);   d += 4; break;
        case '>': memcpy(d, "&gt;", 4);   d += 4; break;
        case '"': memcpy(d, "&quot;", 6); d += 6; break;
        default:  *d++ = *s; break;
        }
    }
    return d;
}

static size_t ge_needed(const ge_op_t *ops, uint32_t count, const char *const *slots);
static char  *ge_write(char *d, const char *pool, const ge_op_t *ops, uint32_t count,
                       const char *const *slots);

int ge_run(const char *pool, const ge_op_t *ops, uint32_t count, const char *const *slots,
           ge_str_t *out)
{
    char *buf = (char *)malloc(ge_needed(ops, count, slots) + 1);
    if (!buf) {
        out->data = NULL;
        out->len  = 0;
        return -1;
    }
    char *end = ge_write(buf, pool, ops, count, slots);
    *end = '\0';
    out->data = buf;
    out->len  = (size_t)(end - buf);
    return 0;
}

void ge_str_free(ge_str_t *s)
{
    free(s->data);
    s->data = NULL;
    s->len  = 0;
}

void ge_mail_free(ge_mail_t *m)
{
    ge_str_free(&m->subject);
    ge_str_free(&m->html);
    ge_str_free(&m->text);
}

/* ------------------------------------------------------------------ arena */

int ge_arena_init(ge_arena_t *a, size_t cap)
{
    a->base = (char *)malloc(cap);
    a->cap  = a->base ? cap : 0;
    a->used = 0;
    return a->base ? 0 : -1;
}

int ge_arena_ensure(ge_arena_t *a, size_t need)
{
    if (a->cap >= need) return 0;
    size_t want = a->cap * 2 > need ? a->cap * 2 : need;
    char  *p    = (char *)realloc(a->base, want);
    if (!p) return -1;
    a->base = p;
    a->cap  = want;
    a->used = 0;
    return 0;
}

void ge_arena_free(ge_arena_t *a)
{
    free(a->base);
    a->base = NULL;
    a->cap = a->used = 0;
}

static size_t ge_needed(const ge_op_t *ops, uint32_t count, const char *const *slots)
{
    size_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        const char *v;
        switch (ops[i].len) {
        case GE_SLOT_HTML: v = slots[ops[i].off]; total += v ? ge_esc_len(v) : 0; break;
        case GE_SLOT_RAW:  v = slots[ops[i].off]; total += v ? strlen(v) : 0;     break;
        default:           total += ops[i].len;                                   break;
        }
    }
    return total;
}

/* Writes from 'd' and returns the end. Shared by all three entry points. */
static char *ge_write(char *d, const char *pool, const ge_op_t *ops, uint32_t count,
                      const char *const *slots)
{
    for (uint32_t i = 0; i < count; i++) {
        const char *v;
        size_t      n;
        switch (ops[i].len) {
        case GE_SLOT_HTML:
            v = slots[ops[i].off];
            if (v) d = ge_esc_put(d, v);
            break;
        case GE_SLOT_RAW:
            v = slots[ops[i].off];
            if (v) { n = strlen(v); memcpy(d, v, n); d += n; }
            break;
        default:
            memcpy(d, pool + ops[i].off, ops[i].len);
            d += ops[i].len;
            break;
        }
    }
    return d;
}

int ge_run_into(const char *pool, const ge_op_t *ops, uint32_t count, const char *const *slots,
                ge_arena_t *a, ge_str_t *out)
{
    size_t need = ge_needed(ops, count, slots) + 1;
    if (a->cap - a->used < need) {
        out->data = NULL;
        out->len  = need;
        return -1;
    }
    char *start = a->base + a->used;
    char *end   = ge_write(start, pool, ops, count, slots);
    *end = '\0';
    a->used += (size_t)(end - start) + 1;
    out->data = start;
    out->len  = (size_t)(end - start);
    return 0;
}

int ge_run_into_fast(const char *pool, const ge_op_t *ops, uint32_t count,
                     const char *const *slots, ge_arena_t *a, ge_str_t *out)
{
    char *start = a->base + a->used;
    char *end   = ge_write(start, pool, ops, count, slots);
    *end = '\0';
    a->used += (size_t)(end - start) + 1;
    out->data = start;
    out->len  = (size_t)(end - start);
    return 0;
}
