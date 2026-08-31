/*
 * Node-API surface for the two C halves of this package:
 *
 *   the renderer   src/email.c + the generated table
 *   the mailer     service-core's sc_mailer, libcurl over mbedTLS
 *
 * Nothing here calls back into JS from a foreign thread, and that is what keeps
 * it small: sc_mail_enqueue does not block and does not report, so there is no
 * threadsafe function, no async work and no interaction with the event loop.
 * The worker threads belong to sc_mailer and never see a napi_env.
 */
#include <node_api.h>

#include <stdlib.h>
#include <string.h>

#include "service_core/email_gen.h"
#ifdef GE_WITH_MAILER
#include "service_core/mail.h"
#endif

#define MAX_SLOTS 32
#define MAX_FLAGS 8

#define CHECK(expr)                                                                               \
    do {                                                                                          \
        if ((expr) != napi_ok) return NULL;                                                       \
    } while (0)

static napi_value throw_msg(napi_env env, const char *msg)
{
    napi_throw_error(env, NULL, msg);
    return NULL;
}

/* ------------------------------------------------------------------ strings */

/* Reads a JS string into fresh memory. NULL for null/undefined, which is how a
 * caller says "this slot is not set" -- and what the if-branches read. */
static char *js_string(napi_env env, napi_value v)
{
    napi_valuetype type;
    if (napi_typeof(env, v, &type) != napi_ok) return NULL;
    if (type != napi_string) return NULL;

    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, NULL, 0, &len) != napi_ok) return NULL;
    char *s = (char *)malloc(len + 1);
    if (!s) return NULL;
    if (napi_get_value_string_utf8(env, v, s, len + 1, &len) != napi_ok) {
        free(s);
        return NULL;
    }
    return s;
}

static char *js_prop_string(napi_env env, napi_value obj, const char *name)
{
    napi_value v;
    if (napi_get_named_property(env, obj, name, &v) != napi_ok) return NULL;
    return js_string(env, v);
}

static bool js_prop_bool(napi_env env, napi_value obj, const char *name)
{
    napi_value v;
    bool       out = false;
    if (napi_get_named_property(env, obj, name, &v) != napi_ok) return false;
    napi_valuetype type;
    if (napi_typeof(env, v, &type) != napi_ok || type != napi_boolean) return false;
    return napi_get_value_bool(env, v, &out) == napi_ok ? out : false;
}

static uint32_t js_prop_u32(napi_env env, napi_value obj, const char *name, uint32_t fallback)
{
    napi_value v;
    if (napi_get_named_property(env, obj, name, &v) != napi_ok) return fallback;
    napi_valuetype type;
    if (napi_typeof(env, v, &type) != napi_ok || type != napi_number) return fallback;
    uint32_t out = fallback;
    return napi_get_value_uint32(env, v, &out) == napi_ok ? out : fallback;
}

/* ------------------------------------------------------------------- arena */

/* One arena for the whole addon. JS is single threaded, and every render is
 * finished -- copied into JS strings or handed to sc_mail_enqueue, which
 * copies -- before the next one starts. */
static ge_arena_t g_arena;
static bool       g_arena_ready;

static bool arena_ready(void)
{
    if (!g_arena_ready) {
        if (ge_arena_init(&g_arena, GE_BUF_SIZE(512)) != 0) return false;
        g_arena_ready = true;
    }
    ge_arena_reset(&g_arena);
    return true;
}

/* Fills slots and flags from a JS object, using the template's own names.
 * Frees nothing: the caller owns `owned` and releases it. */
static bool collect_values(napi_env env, const ge_template_info_t *ti, napi_value values,
                           const char **sv, bool *flags, char **owned)
{
    for (uint32_t i = 1; i < ti->n_slots; i++) {
        owned[i] = js_prop_string(env, values, ti->slot_names[i]);
        sv[i] = owned[i];
    }
    for (uint32_t j = 0; j < ti->n_flags; j++) flags[j] = js_prop_bool(env, values, ti->flag_names[j]);
    return true;
}

static void free_owned(char **owned, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) free(owned[i]);
}

/* Resolves the first two arguments every render-ish call takes. */
static bool resolve(napi_env env, napi_value tpl_v, napi_value loc_v, int *tpl, int *loc)
{
    char *tpl_name = js_string(env, tpl_v);
    char *loc_code = js_string(env, loc_v);
    *tpl = tpl_name ? ge_template_by_name(tpl_name) : -1;
    *loc = loc_code ? ge_locale_by_code(loc_code) : -1;
    free(tpl_name);
    free(loc_code);
    return *tpl >= 0 && *loc >= 0;
}

/* ------------------------------------------------------------------ render */

static napi_value fn_render(napi_env env, napi_callback_info info)
{
    size_t     argc = 3;
    napi_value argv[3];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 3) return throw_msg(env, "render(template, locale, values)");

    int tpl, loc;
    if (!resolve(env, argv[0], argv[1], &tpl, &loc))
        return throw_msg(env, "unknown template or locale");

    const ge_template_info_t *ti = &GE_TEMPLATES[tpl];
    const char *sv[MAX_SLOTS];
    char       *owned[MAX_SLOTS] = { 0 };
    bool        flags[MAX_FLAGS] = { 0 };
    collect_values(env, ti, argv[2], sv, flags, owned);

    if (!arena_ready()) {
        free_owned(owned, ti->n_slots);
        return throw_msg(env, "out of memory");
    }

    ge_mail_t mail;
    int       rc = ge_render_values_into((ge_template_t)tpl, (ge_locale_t)loc, sv, flags, &g_arena,
                                         &mail);
    if (rc != 0) {
        /* mail.html.len carries what it would have needed. */
        if (ge_arena_ensure(&g_arena, mail.html.len) == 0)
            rc = ge_render_values_into((ge_template_t)tpl, (ge_locale_t)loc, sv, flags, &g_arena,
                                       &mail);
    }
    free_owned(owned, ti->n_slots);
    if (rc != 0) return throw_msg(env, "render failed");

    napi_value out, subject, html;
    CHECK(napi_create_object(env, &out));
    CHECK(napi_create_string_utf8(env, mail.subject.data, mail.subject.len, &subject));
    CHECK(napi_create_string_utf8(env, mail.html.data, mail.html.len, &html));
    CHECK(napi_set_named_property(env, out, "subject", subject));
    CHECK(napi_set_named_property(env, out, "html", html));
    return out;
}

/* The same render, handed back as Buffers. napi_create_string_utf8 has to
 * validate 21 KB of UTF-8 and turn it into the engine's own representation;
 * a Buffer is a memcpy. A caller that only forwards the bytes to an SMTP
 * layer never needs them to be a JS string. */
static napi_value fn_render_bytes(napi_env env, napi_callback_info info)
{
    size_t     argc = 3;
    napi_value argv[3];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 3) return throw_msg(env, "renderBytes(template, locale, values)");

    int tpl, loc;
    if (!resolve(env, argv[0], argv[1], &tpl, &loc))
        return throw_msg(env, "unknown template or locale");

    const ge_template_info_t *ti = &GE_TEMPLATES[tpl];
    const char *sv[MAX_SLOTS];
    char       *owned[MAX_SLOTS] = { 0 };
    bool        flags[MAX_FLAGS] = { 0 };
    collect_values(env, ti, argv[2], sv, flags, owned);

    if (!arena_ready()) {
        free_owned(owned, ti->n_slots);
        return throw_msg(env, "out of memory");
    }
    ge_mail_t mail;
    int rc = ge_render_values_into((ge_template_t)tpl, (ge_locale_t)loc, sv, flags, &g_arena, &mail);
    if (rc != 0 && ge_arena_ensure(&g_arena, mail.html.len) == 0)
        rc = ge_render_values_into((ge_template_t)tpl, (ge_locale_t)loc, sv, flags, &g_arena, &mail);
    free_owned(owned, ti->n_slots);
    if (rc != 0) return throw_msg(env, "render failed");

    napi_value out, subject, html;
    void      *copy;
    CHECK(napi_create_object(env, &out));
    CHECK(napi_create_buffer_copy(env, mail.subject.len, mail.subject.data, &copy, &subject));
    CHECK(napi_create_buffer_copy(env, mail.html.len, mail.html.data, &copy, &html));
    CHECK(napi_set_named_property(env, out, "subject", subject));
    CHECK(napi_set_named_property(env, out, "html", html));
    return out;
}

#ifdef GE_WITH_MAILER
/* --------------------------------------------------------------- mailer */

/* Wrapped so that close() and the finalizer cannot both destroy it. */
typedef struct {
    sc_mailer *m;
} mailer_box;

static void finalize_mailer(napi_env env, void *data, void *hint)
{
    (void)env;
    (void)hint;
    mailer_box *box = (mailer_box *)data;
    if (box->m) sc_mailer_destroy(box->m);
    free(box);
}

static mailer_box *unwrap(napi_env env, napi_value v)
{
    void *p = NULL;
    if (napi_get_value_external(env, v, &p) != napi_ok) return NULL;
    return (mailer_box *)p;
}

static napi_value fn_create_mailer(napi_env env, napi_callback_info info)
{
    size_t     argc = 1;
    napi_value argv[1];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 1) return throw_msg(env, "createMailer(config)");

    char *url = js_prop_string(env, argv[0], "url");
    char *from = js_prop_string(env, argv[0], "from");
    char *from_name = js_prop_string(env, argv[0], "fromName");
    char *user = js_prop_string(env, argv[0], "user");
    char *pass = js_prop_string(env, argv[0], "pass");
    char *cainfo = js_prop_string(env, argv[0], "cainfo");

    sc_mail_config cfg = {
        .url = url,
        .from = from,
        .from_name = from_name,
        .user = user,
        .pass = pass,
        .cainfo = cainfo,
        .starttls = (int)js_prop_u32(env, argv[0], "starttls", 1),
        .insecure = js_prop_bool(env, argv[0], "insecure") ? 1 : 0,
        .workers = js_prop_u32(env, argv[0], "workers", 1),
        /* Worth naming rather than leaving at 0: the default asks libuv how many
         * cores the machine has, and Bun's N-API libuv shim has no
         * uv_available_parallelism yet -- oven-sh/bun#18546. A 0 here crashes
         * the Bun process on createMailer. */
        .worker_max = js_prop_u32(env, argv[0], "workerMax", 2),
        .timeout_ms = (long)js_prop_u32(env, argv[0], "timeoutMs", 0),
        .queue_max = js_prop_u32(env, argv[0], "queueMax", 0),
        .message_max = js_prop_u32(env, argv[0], "messageMax", 0),
    };

    sc_mailer *m = NULL;
    sc_status  st = sc_mailer_create(&cfg, &m);
    free(url);
    free(from);
    free(from_name);
    free(user);
    free(pass);
    free(cainfo);
    if (st != SC_OK) return throw_msg(env, "sc_mailer_create failed");

    mailer_box *box = (mailer_box *)malloc(sizeof *box);
    if (!box) {
        sc_mailer_destroy(m);
        return throw_msg(env, "out of memory");
    }
    box->m = m;

    napi_value ext;
    CHECK(napi_create_external(env, box, finalize_mailer, NULL, &ext));
    return ext;
}

static napi_value fn_close_mailer(napi_env env, napi_callback_info info)
{
    size_t     argc = 1;
    napi_value argv[1];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    mailer_box *box = argc ? unwrap(env, argv[0]) : NULL;
    if (!box) return throw_msg(env, "close(mailer)");
    if (box->m) {
        sc_mailer_destroy(box->m);
        box->m = NULL;
    }
    return NULL;
}

static napi_value fn_enqueue(napi_env env, napi_callback_info info)
{
    size_t     argc = 2;
    napi_value argv[2];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 2) return throw_msg(env, "enqueue(mailer, {to, subject, body})");

    mailer_box *box = unwrap(env, argv[0]);
    if (!box || !box->m) return throw_msg(env, "mailer is closed");

    char   *to = js_prop_string(env, argv[1], "to");
    char   *subject = js_prop_string(env, argv[1], "subject");
    char   *body = js_prop_string(env, argv[1], "body");
    sc_mail mail = { .to = to, .subject = subject, .body = body };
    sc_status st = (to && subject && body) ? sc_mail_enqueue(box->m, &mail)
                                           : SC_ERR_INVALID_ARGUMENT;
    free(to);
    free(subject);
    free(body);
    if (st != SC_OK) return throw_msg(env, "sc_mail_enqueue failed");
    return NULL;
}

/* The shape the package exists for: render straight out of the byte pool
 * into the arena, then hand the bytes to the mailer, which copies them into its
 * queue. No intermediate JS string, and no allocation on either side. */
static napi_value fn_send_template(napi_env env, napi_callback_info info)
{
    size_t     argc = 5;
    napi_value argv[5];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 5) return throw_msg(env, "sendTemplate(mailer, to, template, locale, values)");

    mailer_box *box = unwrap(env, argv[0]);
    if (!box || !box->m) return throw_msg(env, "mailer is closed");

    int tpl, loc;
    if (!resolve(env, argv[2], argv[3], &tpl, &loc))
        return throw_msg(env, "unknown template or locale");

    const ge_template_info_t *ti = &GE_TEMPLATES[tpl];
    const char *sv[MAX_SLOTS];
    char       *owned[MAX_SLOTS] = { 0 };
    bool        flags[MAX_FLAGS] = { 0 };
    collect_values(env, ti, argv[4], sv, flags, owned);

    char *to = js_string(env, argv[1]);
    if (!to || !arena_ready()) {
        free(to);
        free_owned(owned, ti->n_slots);
        return throw_msg(env, "out of memory or missing recipient");
    }

    ge_mail_t rendered;
    int rc = ge_render_values_into((ge_template_t)tpl, (ge_locale_t)loc, sv, flags, &g_arena,
                                   &rendered);
    if (rc != 0 && ge_arena_ensure(&g_arena, rendered.html.len) == 0)
        rc = ge_render_values_into((ge_template_t)tpl, (ge_locale_t)loc, sv, flags, &g_arena,
                                   &rendered);
    free_owned(owned, ti->n_slots);
    if (rc != 0) {
        free(to);
        return throw_msg(env, "render failed");
    }

    sc_mail   mail = { .to = to, .subject = rendered.subject.data, .body = rendered.html.data };
    sc_status st = sc_mail_enqueue(box->m, &mail);
    free(to);
    if (st != SC_OK) return throw_msg(env, "sc_mail_enqueue failed");
    return NULL;
}

static napi_value fn_stats(napi_env env, napi_callback_info info)
{
    size_t     argc = 1;
    napi_value argv[1];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    mailer_box *box = argc ? unwrap(env, argv[0]) : NULL;
    if (!box || !box->m) return throw_msg(env, "mailer is closed");

    sc_mail_stats s;
    sc_mail_get_stats(box->m, &s);

    napi_value out, v;
    CHECK(napi_create_object(env, &out));
#define PUT(name, value)                                                                          \
    CHECK(napi_create_double(env, (double)(value), &v));                                          \
    CHECK(napi_set_named_property(env, out, name, v))
    PUT("queued", s.queued);
    PUT("sent", s.sent);
    PUT("retried", s.retried);
    PUT("failed", s.failed);
    PUT("pending", s.pending);
    PUT("workers", s.workers);
#undef PUT
    return out;
}

static napi_value fn_drain(napi_env env, napi_callback_info info)
{
    size_t     argc = 2;
    napi_value argv[2];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    mailer_box *box = argc ? unwrap(env, argv[0]) : NULL;
    if (!box || !box->m) return throw_msg(env, "mailer is closed");
    /* Blocks, so this is a test and shutdown helper -- not something a request
     * handler calls. A production path would wrap it in napi_create_async_work. */
    int32_t ms = (int32_t)js_prop_u32(env, argv[1], "timeoutMs", 5000);
    napi_value out;
    CHECK(napi_get_boolean(env, sc_mail_drain(box->m, ms) == SC_OK, &out));
    return out;
}

#endif /* GE_WITH_MAILER */

/* --------------------------------------------------------- introspection */

static napi_value string_array(napi_env env, const char *const *items, uint32_t n)
{
    napi_value arr, s;
    CHECK(napi_create_array_with_length(env, n, &arr));
    for (uint32_t i = 0; i < n; i++) {
        CHECK(napi_create_string_utf8(env, items[i], NAPI_AUTO_LENGTH, &s));
        CHECK(napi_set_element(env, arr, i, s));
    }
    return arr;
}

static napi_value fn_templates(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value arr;
    CHECK(napi_create_array_with_length(env, GE_TPL_COUNT, &arr));
    for (int i = 0; i < GE_TPL_COUNT; i++) {
        const ge_template_info_t *ti = &GE_TEMPLATES[i];
        napi_value                o, v;
        CHECK(napi_create_object(env, &o));
        CHECK(napi_create_string_utf8(env, ti->name, NAPI_AUTO_LENGTH, &v));
        CHECK(napi_set_named_property(env, o, "name", v));
        /* slot_names[0] is "locale", which the renderer fills; a caller never passes it. */
        CHECK(napi_set_named_property(env, o, "slots",
                                      string_array(env, ti->slot_names + 1, ti->n_slots - 1)));
        CHECK(napi_set_named_property(env, o, "flags",
                                      string_array(env, ti->flag_names, ti->n_flags)));
        CHECK(napi_create_uint32(env, ti->n_combos, &v));
        CHECK(napi_set_named_property(env, o, "variants", v));
        CHECK(napi_set_element(env, arr, (uint32_t)i, o));
    }
    return arr;
}

static napi_value fn_locales(napi_env env, napi_callback_info info)
{
    (void)info;
    const char *codes[GE_LOCALE_COUNT];
    for (int i = 0; i < GE_LOCALE_COUNT; i++) codes[i] = ge_locale_code((ge_locale_t)i);
    return string_array(env, codes, GE_LOCALE_COUNT);
}

static napi_value fn_assets(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value arr;
    CHECK(napi_create_array_with_length(env, GE_ASSET_COUNT, &arr));
    for (int i = 0; i < GE_ASSET_COUNT; i++) {
        napi_value o, v;
        CHECK(napi_create_object(env, &o));
        CHECK(napi_create_string_utf8(env, GE_ASSETS[i].cid, NAPI_AUTO_LENGTH, &v));
        CHECK(napi_set_named_property(env, o, "cid", v));
        CHECK(napi_create_string_utf8(env, GE_ASSETS[i].filename, NAPI_AUTO_LENGTH, &v));
        CHECK(napi_set_named_property(env, o, "filename", v));
        CHECK(napi_create_string_utf8(env, GE_ASSETS[i].content_type, NAPI_AUTO_LENGTH, &v));
        CHECK(napi_set_named_property(env, o, "contentType", v));
        CHECK(napi_create_uint32(env, (uint32_t)GE_ASSETS[i].size, &v));
        CHECK(napi_set_named_property(env, o, "size", v));
        /* The bytes themselves, so a JS sender can attach them without a second copy on disk. */
        napi_value buf;
        void      *copy;
        CHECK(napi_create_buffer_copy(env, GE_ASSETS[i].size, GE_ASSETS[i].data, &copy, &buf));
        CHECK(napi_set_named_property(env, o, "data", buf));
        CHECK(napi_set_element(env, arr, (uint32_t)i, o));
    }
    return arr;
}

/* The build-time constants behind the buffer sizing, so a JS caller can see
 * them rather than guess. */
static napi_value fn_limits(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out, v;
    CHECK(napi_create_object(env, &out));
#define PUT(name, value)                                                                          \
    CHECK(napi_create_uint32(env, (uint32_t)(value), &v));                                        \
    CHECK(napi_set_named_property(env, out, name, v))
    PUT("maxStaticHtml", GE_MAX_STATIC_HTML);
    PUT("maxStaticSubject", GE_MAX_STATIC_SUBJECT);
    PUT("maxSlotRefs", GE_MAX_SLOT_REFS);
    PUT("arenaBytes", GE_BUF_SIZE(512));
#undef PUT
    return out;
}

/* ------------------------------------------------------------------- init */

static void cleanup(void *arg)
{
    (void)arg;
    if (g_arena_ready) {
        ge_arena_free(&g_arena);
        g_arena_ready = false;
    }
}

static napi_value Init(napi_env env, napi_value exports)
{
    const struct {
        const char  *name;
        napi_callback fn;
    } entries[] = {
        { "render", fn_render },
        { "renderBytes", fn_render_bytes },
        { "templates", fn_templates },
        { "locales", fn_locales },
        { "assets", fn_assets },
        { "limits", fn_limits },
#ifdef GE_WITH_MAILER
        { "createMailer", fn_create_mailer },
        { "closeMailer", fn_close_mailer },
        { "enqueue", fn_enqueue },
        { "sendTemplate", fn_send_template },
        { "stats", fn_stats },
        { "drain", fn_drain },
#endif
    };
    for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++) {
        napi_value fn;
        CHECK(napi_create_function(env, entries[i].name, NAPI_AUTO_LENGTH, entries[i].fn, NULL,
                                   &fn));
        CHECK(napi_set_named_property(env, exports, entries[i].name, fn));
    }
    /* The arena outlives every call but not the environment. */
    napi_add_env_cleanup_hook(env, cleanup, NULL);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
