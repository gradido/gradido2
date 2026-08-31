/* Renders everything that is in the binary with fixed test values into a
 * directory. Counterpart to tools/verify.mjs. */
#include "service_core/email/templates.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SLOTS 32

static void write_file(const char *dir, const char *tpl, const char *loc, unsigned combo,
                       const char *ext, const ge_str_t *s)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.%s.%u.%s", dir, tpl, loc, combo, ext);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(s->data, 1, s->len, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "out/c";
    size_t      bytes = 0;
    unsigned    n = 0;

    for (int t = 0; t < GE_TPL_COUNT; t++) {
        const ge_template_info_t *ti = &GE_TEMPLATES[t];
        const char *sv[MAX_SLOTS];
        char        buf[MAX_SLOTS][128];

        if (ti->n_slots > MAX_SLOTS) { fprintf(stderr, "MAX_SLOTS too small\n"); return 1; }
        sv[0] = NULL; /* the renderer sets this one */
        for (uint32_t s = 1; s < ti->n_slots; s++) {
            /* identical to fixture() in tools/verify.mjs */
            snprintf(buf[s], sizeof buf[s], "{%s|&<>\"'\xc3\xa4}", ti->slot_names[s]);
            sv[s] = buf[s];
        }

        for (int l = 0; l < GE_LOCALE_COUNT; l++) {
            for (uint32_t c = 0; c < ti->n_combos; c++) {
                ge_mail_t m;
                if (ge_render_by_index((ge_template_t)t, (ge_locale_t)l, c, sv, &m) != 0) {
                    fprintf(stderr, "render failed: %s/%s/%u\n", ti->name, ge_locale_code(l), c);
                    return 1;
                }
                if (m.html.len != strlen(m.html.data)) {
                    fprintf(stderr, "embedded NUL in %s\n", ti->name);
                    return 1;
                }
                write_file(dir, ti->name, ge_locale_code(l), c, "html", &m.html);
                write_file(dir, ti->name, ge_locale_code(l), c, "subject", &m.subject);
                write_file(dir, ti->name, ge_locale_code(l), c, "text", &m.text);
                bytes += m.html.len + m.subject.len + m.text.len;
                n += 3;
                ge_mail_free(&m);
            }
        }
    }
    fprintf(stderr, "%u documents, %.1f KB rendered\n", n, bytes / 1024.0);
    return 0;
}
