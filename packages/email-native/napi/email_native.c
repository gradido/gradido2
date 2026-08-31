/*
 * Node-API surface for the two C halves of this package:
 *
 *   the renderer   src/render.c + the generated table
 *   the mailer     service-core's email/message.c and email/transport.c, libcurl over mbedTLS
 *
 * **One connection per mail, on a libuv thread pool thread.** The addon does not use
 * service-core's sc_mailer: that one holds sessions open and runs a worker pool of its own,
 * which is what a server sending thousands of mails a second wants and not what a Node process
 * sending one per request wants. Here every send is a napi_async_work -- execute() opens a
 * session, hands over one message and closes it, complete() settles a Promise on the JS thread.
 * The same shape nodemailer has by default, and the reason this file links no libuv at all.
 *
 * The cost is named rather than hidden: a new session per mail is a TCP handshake, a TLS
 * handshake and the SMTP greeting dialogue, so against a remote relay it is round trips and not
 * microseconds. And the pool has four threads by default (UV_THREADPOOL_SIZE), shared with
 * fs, dns and crypto -- four mails may be in flight, each holding its thread for the whole
 * session. For registration and notification mail that is the right trade; for a newsletter it
 * is what fast-servers exists for.
 *
 * execute() runs on a thread with no napi_env: nothing in it may call napi_*, which is why a job
 * carries everything it needs and reports back through plain C fields.
 */
#include <node_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "service_core/email/templates.h"
#ifdef GE_WITH_MAILER
#include "service_core/email/message.h"
#include "service_core/email/transport.h"
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

/* One arena for the whole addon. JS is single threaded, and every render is finished --
 * copied into JS strings, or formatted into the job's own buffer -- before the next one
 * starts. Nothing a pool thread touches lives in here. */
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

    napi_value out, subject, html, text;
    CHECK(napi_create_object(env, &out));
    CHECK(napi_create_string_utf8(env, mail.subject.data, mail.subject.len, &subject));
    CHECK(napi_create_string_utf8(env, mail.html.data, mail.html.len, &html));
    CHECK(napi_create_string_utf8(env, mail.text.data, mail.text.len, &text));
    CHECK(napi_set_named_property(env, out, "subject", subject));
    CHECK(napi_set_named_property(env, out, "html", html));
    CHECK(napi_set_named_property(env, out, "text", text));
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

    napi_value out, subject, html, text;
    void      *copy;
    CHECK(napi_create_object(env, &out));
    CHECK(napi_create_buffer_copy(env, mail.subject.len, mail.subject.data, &copy, &subject));
    CHECK(napi_create_buffer_copy(env, mail.html.len, mail.html.data, &copy, &html));
    CHECK(napi_create_buffer_copy(env, mail.text.len, mail.text.data, &copy, &text));
    CHECK(napi_set_named_property(env, out, "subject", subject));
    CHECK(napi_set_named_property(env, out, "html", html));
    CHECK(napi_set_named_property(env, out, "text", text));
    return out;
}

#ifdef GE_WITH_MAILER
/* --------------------------------------------------------------- mailer */

/*
 * The relay, as JavaScript described it, with the strings copied.
 *
 * `relay` and `origin` point into the char pointers below, so the box is what keeps them alive
 * and nothing here may be moved. Every field is read on the JS thread except `relay`, which a
 * pool thread reads while a send runs -- it is written once, at create, and never again.
 *
 * `inflight` and the counters are touched only on the JS thread (send queues, complete settles),
 * so they need no atomics. `closed` is what makes close() safe while a send is out: it refuses
 * new sends, and whoever sees the last job finish frees the box.
 */
typedef struct {
    sc_mail_relay  relay;
    sc_mail_origin origin;
    char    *url, *from, *from_name, *user, *pass, *cainfo;
    uint64_t sequence;
    uint32_t inflight;
    uint32_t sent, failed;
    bool     closed;
} mailer_box;

/* One send in flight. Owns its copies, because the JS values are gone by the time it runs. */
typedef struct {
    mailer_box     *box;
    napi_deferred   deferred;
    napi_async_work work;
    char           *to;
    char           *message;
    size_t          len;
    char            msgid[SC_MAIL_MSGID_MAX];
    sc_status       status;
    char            error[SC_MAIL_ERROR_MAX];
} send_job;

static void mailer_free(mailer_box *box)
{
    free(box->url);
    free(box->from);
    free(box->from_name);
    free(box->user);
    free(box->pass);
    free(box->cainfo);
    free(box);
}

static void finalize_mailer(napi_env env, void *data, void *hint)
{
    (void)env;
    (void)hint;
    mailer_box *box = (mailer_box *)data;
    box->closed = true;
    /* A job still in flight holds the box; its completion frees it. */
    if (box->inflight == 0) mailer_free(box);
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

    mailer_box *box = (mailer_box *)calloc(1, sizeof *box);
    if (!box) return throw_msg(env, "out of memory");

    box->url = js_prop_string(env, argv[0], "url");
    box->from = js_prop_string(env, argv[0], "from");
    box->from_name = js_prop_string(env, argv[0], "fromName");
    box->user = js_prop_string(env, argv[0], "user");
    box->pass = js_prop_string(env, argv[0], "pass");
    box->cainfo = js_prop_string(env, argv[0], "cainfo");
    if (!box->url || !box->from) {
        mailer_free(box);
        return throw_msg(env, "createMailer needs at least { url, from }");
    }

    box->relay.url = box->url;
    box->relay.from = box->from;
    box->relay.user = box->user;
    box->relay.pass = box->pass;
    box->relay.cainfo = box->cainfo;
    box->relay.starttls = (int)js_prop_u32(env, argv[0], "starttls", 1);
    box->relay.insecure = js_prop_bool(env, argv[0], "insecure") ? 1 : 0;
    box->relay.timeout_ms = (long)js_prop_u32(env, argv[0], "timeoutMs", 0);
    box->origin.from = box->from;
    box->origin.from_name = box->from_name;
    box->origin.msgid_domain = NULL; /* email/message.c takes it out of `from` */

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
    /* Sends already out keep running and settle their promises; the last one frees the box.
     * Nothing is cancelled -- a mail the relay may already have taken is not a thing to
     * pretend never happened. */
    box->closed = true;
    return NULL;
}

/* execute(): a pool thread, no napi_env, no napi_* call. */
static void job_execute(napi_env env, void *data)
{
    (void)env;
    send_job        *job = (send_job *)data;
    sc_mail_session *session = sc_mail_session_open();
    if (!session) {
        job->status = SC_ERR_NO_MEMORY;
        snprintf(job->error, sizeof job->error, "no SMTP session");
        return;
    }
    job->status = sc_mail_session_send(session, &job->box->relay, job->to, job->message, job->len,
                                       job->error, sizeof job->error);
    sc_mail_session_close(session);
}

/* complete(): back on the JS thread, so this is where the promise is settled. */
static void job_complete(napi_env env, napi_status status, void *data)
{
    send_job   *job = (send_job *)data;
    mailer_box *box = job->box;
    napi_value  value;

    if (status == napi_ok && job->status == SC_OK) {
        box->sent++;
        /* The Message-ID, because it is the one identifier that reaches the relay's log too. */
        if (napi_create_string_utf8(env, job->msgid, NAPI_AUTO_LENGTH, &value) == napi_ok)
            napi_resolve_deferred(env, job->deferred, value);
    } else {
        char message[SC_MAIL_ERROR_MAX + SC_MAIL_ADDR_MAX + 32];
        box->failed++;
        snprintf(message, sizeof message, "%s: %s", job->to,
                 job->error[0] != '\0' ? job->error : "send failed");
        if (napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &value) == napi_ok) {
            napi_value error;
            if (napi_create_error(env, NULL, value, &error) == napi_ok)
                napi_reject_deferred(env, job->deferred, error);
        }
    }

    napi_delete_async_work(env, job->work);
    free(job->to);
    free(job->message);
    free(job);
    if (--box->inflight == 0 && box->closed) mailer_free(box);
}

/*
 * The six inline images, in the shape email/message.h wants them.
 *
 * Built once from the generated table: the bytes are in the binary already, and the two structs
 * differ only because a message is not a thing that knows about templates.
 */
static sc_mail_asset g_assets[GE_ASSET_COUNT];
static bool          g_assets_ready;

static const sc_mail_asset *assets_for_templates(void)
{
    if (!g_assets_ready) {
        for (unsigned i = 0; i < GE_ASSET_COUNT; i++) {
            g_assets[i].cid = GE_ASSETS[i].cid;
            g_assets[i].filename = GE_ASSETS[i].filename;
            g_assets[i].content_type = GE_ASSETS[i].content_type;
            g_assets[i].data = GE_ASSETS[i].data;
            g_assets[i].size = GE_ASSETS[i].size;
        }
        g_assets_ready = true;
    }
    return g_assets;
}

/**
 * Formats one mail and queues the send. Returns the promise, or NULL after throwing.
 *
 * Takes ownership of @p to whatever happens, and of nothing else: the subject and the two
 * bodies are read here and may be arena memory the next render overwrites.
 */
static napi_value queue_send(napi_env env, mailer_box *box, char *to, const char *subject,
                             const char *text, const char *html, const sc_mail_asset *assets,
                             uint32_t asset_count)
{
    sc_mail         mail = {to, subject, text, html, assets, asset_count};
    sc_mail_message formatted;
    send_job       *job;
    napi_value      promise, name;
    size_t          cap;

    /*
     * One guess, then the exact figure: sc_mail_format() reports what it would have needed.
     *
     * The guess allows for what the encodings cost -- quoted-printable is at worst three bytes
     * per byte, base64 is four per three -- so the second attempt is for the pathological case
     * rather than the normal one.
     */
    cap = 4096;
    if (text != NULL) cap += 3 * strlen(text);
    if (html != NULL) cap += 3 * strlen(html);
    for (uint32_t i = 0; i < asset_count; i++) cap += (assets[i].size * 4) / 3 + 256;
    job = (send_job *)calloc(1, sizeof *job);
    char *buffer = (char *)malloc(cap);
    if (!job || !buffer) {
        free(job);
        free(buffer);
        free(to);
        return throw_msg(env, "out of memory");
    }

    sc_status st = sc_mail_format(&box->origin, &mail, ++box->sequence, (int64_t)time(NULL) * 1000,
                                  buffer, cap, &formatted);
    if (st == SC_ERR_TOO_LONG && formatted.len > cap) {
        char *grown = (char *)realloc(buffer, formatted.len);
        if (grown) {
            buffer = grown;
            cap = formatted.len;
            st = sc_mail_format(&box->origin, &mail, box->sequence,
                                (int64_t)time(NULL) * 1000, buffer, cap, &formatted);
        }
    }
    if (st != SC_OK) {
        /* A rejected promise and not a throw: this is the mail being wrong, the same class of
         * failure as the relay refusing it, and a caller writing `send(...).catch(...)` must not
         * have one of the two arrive as a synchronous exception instead. */
        napi_deferred rejected;
        napi_value    error, text;
        free(job);
        free(buffer);
        free(to);
        CHECK(napi_create_promise(env, &rejected, &promise));
        CHECK(napi_create_string_utf8(env,
                                      st == SC_ERR_MALFORMED
                                          ? "a control character in the recipient, the sender, "
                                            "the display name or the subject"
                                          : "the mail does not fit",
                                      NAPI_AUTO_LENGTH, &text));
        CHECK(napi_create_error(env, NULL, text, &error));
        CHECK(napi_reject_deferred(env, rejected, error));
        return promise;
    }

    job->box = box;
    job->to = to;
    job->message = buffer;
    job->len = formatted.len;
    memcpy(job->msgid, formatted.msgid, sizeof job->msgid);

    CHECK(napi_create_promise(env, &job->deferred, &promise));
    CHECK(napi_create_string_utf8(env, "email-native:send", NAPI_AUTO_LENGTH, &name));
    CHECK(napi_create_async_work(env, NULL, name, job_execute, job_complete, job, &job->work));
    box->inflight++;
    CHECK(napi_queue_async_work(env, job->work));
    return promise;
}

/* A message the caller rendered itself. */
static napi_value fn_send_mail(napi_env env, napi_callback_info info)
{
    size_t     argc = 2;
    napi_value argv[2];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 2) return throw_msg(env, "sendMail(mailer, {to, subject, body})");

    mailer_box *box = unwrap(env, argv[0]);
    if (!box || box->closed) return throw_msg(env, "mailer is closed");

    char *to = js_prop_string(env, argv[1], "to");
    char *subject = js_prop_string(env, argv[1], "subject");
    /* `text` is the plain alternative and `html` the rich one; either alone is a single-part
     * message, both together a multipart/alternative. `body` is what this took before there
     * were two, and still means the text. */
    char *text = js_prop_string(env, argv[1], "text");
    char *html = js_prop_string(env, argv[1], "html");
    if (!text) text = js_prop_string(env, argv[1], "body");

    napi_value out = NULL;
    if (to && subject && (text || html))
        out = queue_send(env, box, to, subject, text, html, NULL, 0); /* takes `to` */
    else {
        free(to);
        out = throw_msg(env, "sendMail needs { to, subject } and text or html");
    }
    free(subject);
    free(text);
    free(html);
    return out;
}

/* The shape the package exists for: render straight out of the byte pool into the arena, then
 * format from there. The document never becomes a JS value. */
static napi_value fn_send_template(napi_env env, napi_callback_info info)
{
    size_t     argc = 5;
    napi_value argv[5];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 5) return throw_msg(env, "sendTemplate(mailer, to, template, locale, values)");

    mailer_box *box = unwrap(env, argv[0]);
    if (!box || box->closed) return throw_msg(env, "mailer is closed");

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

    /* No text alternative yet -- the renderer has no text program, see the README. What this
     * does have is the six images the templates refer to as cid:, which is what makes the HTML
     * arrive whole. */
    /* Both alternatives and the six images: a multipart/alternative whose HTML half is a
     * multipart/related. The text comes out of the same document -- see tools/manifest.mjs. */
    return queue_send(env, box, to, rendered.subject.data, rendered.text.data,
                      rendered.html.data, assets_for_templates(), GE_ASSET_COUNT);
}

static napi_value fn_stats(napi_env env, napi_callback_info info)
{
    size_t     argc = 1;
    napi_value argv[1];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    mailer_box *box = argc ? unwrap(env, argv[0]) : NULL;
    if (!box) return throw_msg(env, "stats(mailer)");

    napi_value out, v;
    CHECK(napi_create_object(env, &out));
#define PUT(name, value)                                                                          \
    CHECK(napi_create_double(env, (double)(value), &v));                                          \
    CHECK(napi_set_named_property(env, out, name, v))
    PUT("sent", box->sent);
    PUT("failed", box->failed);
    PUT("pending", box->inflight);
#undef PUT
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
    PUT("maxStaticText", GE_MAX_STATIC_TEXT);
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
        { "sendMail", fn_send_mail },
        { "sendTemplate", fn_send_template },
        { "stats", fn_stats },
#endif
    };
    for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++) {
        napi_value fn;
        CHECK(napi_create_function(env, entries[i].name, NAPI_AUTO_LENGTH, entries[i].fn, NULL,
                                   &fn));
        CHECK(napi_set_named_property(env, exports, entries[i].name, fn));
    }
#ifdef GE_WITH_MAILER
    /* curl_global_init(), here and not in a worker: it is not thread safe, and the pool threads
     * that open sessions start later. */
    sc_mail_global_init();
#endif
    /* The arena outlives every call but not the environment. */
    napi_add_env_cleanup_hook(env, cleanup, NULL);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
