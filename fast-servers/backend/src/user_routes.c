/*
 * The `user` domain -- contracts/server/backend/user.json.
 *
 * One file per domain, and the file is the whole thing: path, body, status and the call into the
 * interaction that does the work. There is no handler interface in between.
 *
 * The route stays thin all the same. It owns what is HTTP -- which body is accepted, which status
 * is answered -- and nothing else. Everything a second implementation has to reproduce is behind
 * bc_register_account, in backend-core, which is where the reference path put it too.
 *
 * What is *not* thin here is the validation, and that is the price of not having valibot: the
 * reference route declares `body: userCreateRequestSchema` and gets the refusals, the messages
 * and the field names for free. They are contracted answers -- a client that fails validation is
 * told which field and why -- so they are written out rather than approximated. field_rules.h
 * holds the three rules that are about text; what is here is the order they are applied in, which
 * is the order the schema pipes them in.
 */
#include "routes.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arnm/arena.h"
#include "arnm/json_reader.h"

#include "backend_core/backend_core.h"
#include "backend_core/domain/user.h"
#include "field_rules.h"
#include "service_core/api_error.h"
#include "service_core/log.h"

#define ROUTE_PATH "/user/create"

/*
 * The arena the JSON reader draws from, one per loop thread rather than one per request: the
 * request path does not allocate, and a reader needs somewhere to put a document. It is reset
 * before every parse, so nothing survives a request and two requests on one thread never see
 * each other's bytes.
 *
 * The size is what a body of SC_HTTP_MAX_BODY can cost: the parser copies the input and then
 * allocates a value for roughly every two bytes of it. A body that will not fit is refused as an
 * unreadable one -- see the parse below -- rather than truncated.
 */
#if defined(_MSC_VER)
#define BK_THREAD_LOCAL __declspec(thread)
#else
#define BK_THREAD_LOCAL _Thread_local
#endif
#define BK_REQUEST_ARENA (512u * 1024u)
static BK_THREAD_LOCAL _Alignas(8) uint8_t g_arena[BK_REQUEST_ARENA];

#define FOUND(mask, field) (((mask) & (1ull << (field))) != 0)

static int arnm_ok(arnm_result result)
{
    return result == ARNM_SUCCESS || result == ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED;
}

/* --- what a failure is answered with ------------------------------------------------------- */

/**
 * `validation failed for {field}: {reason}` -- contracts/errors/domain.json, VALIDATION_FAILED.
 *
 * The frontend validates against the same field rules first, so anything arriving here is a
 * client that did not: the message names the field for whoever is writing that client, and no
 * member ever sees it.
 */
static int reply_validation(sc_http_req *req, const char *field, const char *reason)
{
    (void)sc_http_reply_validation_failed(req, field, reason);
    return 0;
}

/**
 * What a client is told when the log knows more than it should.
 *
 * The shape of the line is contracted: contracts/logging.json fixes http.request.failed at
 * method, path and status, with the error code in `err`. What went wrong goes in `msg`, which no
 * test compares and which is the only place it belongs -- the client is told nothing but UNKNOWN.
 */
static int reply_unknown(sc_http_req *req, const char *method, const char *why)
{
    sc_log_value data[3] = {SC_LOG_STR("method", method), SC_LOG_STR("path", ROUTE_PATH),
                            SC_LOG_UINT("status", (uint64_t)sc_api_error_status(SC_API_UNKNOWN))};
    sc_log_context log = {0};

    log.err_name = sc_api_error_name(SC_API_UNKNOWN);
    log.err_code = SC_API_UNKNOWN;
    log.data = data;
    log.data_count = 3;
    sc_log_event(SC_LOG_ERROR, SC_CAT_HTTP, "http.request.failed", &log,
                 "unhandled error while serving %s: %s", ROUTE_PATH, why);

    /* Deliberately says nothing: what went wrong is in the log, where it belongs, and not in an
     * answer to whoever caused it. */
    (void)sc_http_reply_unknown(req);
    return 0;
}

/* --- rendering a value the way valibot names it in a message ------------------------------- */

/**
 * A number as `String(n)` would print it: no decimal point on an integer, and the shortest
 * representation that reads back as the same double otherwise.
 *
 * Exact for every integer and for the decimals a form produces. JavaScript's algorithm differs
 * on values no client sends -- denormals, and anything that needs an exponent -- and the message
 * this feeds is read by whoever is writing a client, not by a program.
 */
static void render_number(double value, char *out, size_t out_size)
{
    int precision;

    if (value == (double)(long long)value && value > -1e15 && value < 1e15) {
        (void)snprintf(out, out_size, "%lld", (long long)value);
        return;
    }
    for (precision = 15; precision != 18; ++precision) {
        (void)snprintf(out, out_size, "%.*g", precision, value);
        if (strtod(out, NULL) == value)
            return;
    }
}

/**
 * What the member at @p key is, in the words valibot's `_stringify` would use.
 *
 * Everything but a string, because a string is what the caller was asking for and would not be
 * here. The type is found by asking for it: a walk typed for a boolean answers only for a
 * boolean, and an object or an array is what the two structural reads accept. Nothing is left,
 * so `null` is what remains -- which is right, and is the one answer arrived at by elimination.
 */
static void render_member(arnm_json_value *root, arnm_json_value *value, const char *key, char *out,
                          size_t out_size)
{
    arnm_json_field probe[1];
    uint64_t found = 0;
    bool flag = false;
    double number = 0;
    arnm_json_value *elements[1];

    probe[0] = (arnm_json_field){key, (uint32_t)strlen(key), ARNM_JSON_FIELD_TYPE_BOOL, &flag};
    arnm_json_read_object(root, probe, 1, &found);
    if (FOUND(found, 0)) {
        (void)snprintf(out, out_size, "%s", flag ? "true" : "false");
        return;
    }
    probe[0] = (arnm_json_field){key, (uint32_t)strlen(key), ARNM_JSON_FIELD_TYPE_DOUBLE, &number};
    arnm_json_read_object(root, probe, 1, &found);
    if (FOUND(found, 0)) {
        render_number(number, out, out_size);
        return;
    }
    if (arnm_json_read_array(value, elements, 1, NULL) == ARNM_SUCCESS ||
        arnm_json_read_array(value, elements, 1, NULL) == ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL) {
        (void)snprintf(out, out_size, "Array");
        return;
    }
    {
        /* A one-entry table over a key nothing carries: the walk succeeds on an object and
         * refuses anything else, which is the question being asked. */
        arnm_json_field empty[1] = {ARNM_JSON_FIELD_BOOL("", &flag)};
        if (arnm_json_read_object(value, empty, 1, &found) != ARNM_ERROR_INVALID_ENUM_TYPE) {
            (void)snprintf(out, out_size, "Object");
            return;
        }
    }
    (void)snprintf(out, out_size, "null");
}

/* --- the fields, in the order the schema declares them ------------------------------------- */

/** contracts/db/user_contacts.json -- `email varchar(255)`, counted as valibot counts it. */
#define EMAIL_MAX_LENGTH 255
/** contracts/db/users.json -- `first_name` and `last_name` are `varchar(255)`. */
#define NAME_MAX_LENGTH 255
/* Legacy asks for three characters of a first name and two of a last name. Both are deliberately
 * lenient: names are shorter and stranger than form designers expect. */
#define MIN_FIRST_NAME 3
#define MIN_LAST_NAME 2

typedef struct field_rule {
    const char *key;
    size_t max_length;
    const char *too_long;
    size_t min_length;
    const char *too_short;
    /** Non-zero for the one field the email rule applies to instead of a minimum length. */
    int is_email;
} field_rule;

/* The order is the schema's, because the order is what decides which failure a client is told
 * about: valibot reports the first issue, and the first issue comes from the first entry that
 * fails. `language` is not here -- it is optional and anything unknown becomes the default, so
 * there is no way for it to fail. */
static const field_rule kFields[] = {
    {"firstName", NAME_MAX_LENGTH, "This name is too long", MIN_FIRST_NAME,
     "Please enter at least three characters", 0},
    {"lastName", NAME_MAX_LENGTH, "This name is too long", MIN_LAST_NAME,
     "Please enter at least two characters", 0},
    {"email", EMAIL_MAX_LENGTH, "This email address is too long", 0,
     "Please enter a valid email address", 1},
};

#define FIELD_COUNT ((size_t)(sizeof(kFields) / sizeof(kFields[0])))

/*
 * A value that has been through the rules above, as bytes.
 *
 * 255 characters, and up to three UTF-8 bytes for each of them -- a character outside the basic
 * plane is two of valibot's units and only four bytes, so three per unit is the worst case. A
 * value that does not fit is refused by the maxLength rule before it is copied, so this bound is
 * a consequence of the rule rather than a second one.
 */
#define FIELD_BYTES_MAX (NAME_MAX_LENGTH * 3 + 1)

/**
 * The pipe `packages/shared/src/schemas` declares, in its order: trim, maxLength, nonEmpty, and
 * then either a minimum length or the email rule.
 *
 * Answers NULL when the value is acceptable, and the schema's own message when it is not -- the
 * same message the registration form shows for the same field, because it is the same string.
 */
static const char *apply_field_rule(const field_rule *rule, const char *text, size_t length,
                                    char *out)
{
    size_t begin = 0;
    size_t trimmed = 0;
    size_t units;

    bk_trim(text, length, &begin, &trimmed);
    units = bk_utf16_length(text + begin, trimmed);
    if (units > rule->max_length)
        return rule->too_long;
    if (trimmed == 0)
        return "This field is required";
    if (rule->is_email) {
        if (!bk_is_email(text + begin, trimmed))
            return rule->too_short;
    } else if (units < rule->min_length) {
        return rule->too_short;
    }
    /* The bound above is what makes this fit; the check is here so that a future rule with a
     * wider maximum cannot quietly overrun the buffer. */
    if (trimmed + 1 > FIELD_BYTES_MAX)
        return rule->too_long;
    memcpy(out, text + begin, trimmed);
    out[trimmed] = '\0';
    return NULL;
}

/* --- the handler ---------------------------------------------------------------------------- */

int backend_user_create(sc_http_req *req, void *user_data)
{
    bc_context *context = (bc_context *)user_data;
    const char *body;
    size_t body_len = 0;
    arnm allocator = {0};
    arnm_json_reader reader;
    arnm_json_value *root = NULL;
    arnm_json_value *members[FIELD_COUNT];
    char values[FIELD_COUNT][FIELD_BYTES_MAX];
    char language[BC_LANGUAGE_MAX];
    char error[BC_SQL_ERROR_MAX];
    arnm_result parsed;
    uint64_t present = 0;
    size_t i;

    /* The reference server declares this route for POST only, and Elysia answers anything else
     * as an unmatched route rather than as a wrong method -- so a GET here is the contracted
     * ROUTE_NOT_IMPLEMENTED, not a 405. Answered here rather than by returning -1, because the
     * two HTTP backends disagree about what an unaccepted request becomes: h2o 404s it inside
     * the path it matched, the fallback offers it to the default route. */
    if (!sc_http_method_is(req, "POST"))
        return backend_route_not_implemented(req, user_data);

    body = sc_http_body(req, &body_len);
    if (body == NULL || body_len == 0) {
        /* No body is `undefined` where the schema wanted an object. */
        return reply_validation(req, "body",
                                "Invalid type: Expected Object but received undefined");
    }

    if (!arnm_ok(arnm_init_arena_borrow(&allocator, g_arena, sizeof(g_arena))) ||
        !arnm_ok(arnm_json_reader_init(&reader, &allocator)))
        return reply_unknown(req, "POST", "the request arena could not be prepared");

    parsed = arnm_json_reader_parse(&reader, body, (uint32_t)body_len, false, &root);
    if (parsed != ARNM_SUCCESS) {
        const char *why = arnm_json_reader_error_message(&reader);
        arnm_json_reader_release(&reader);
        /* A body that is not JSON is not a validation failure on the reference path either: it
         * never reaches the schema, so Elysia raises PARSE, the route's error handler does not
         * name that code, and the client is told UNKNOWN. Mirrored rather than improved on --
         * two implementations answering one request differently is the thing this path may not
         * do. A body too large for the arena lands here too, with its own sentence in the log. */
        return reply_unknown(req, "POST", why);
    }

    /* Which members are there, whatever their type. VALUE accepts every one of them, so this
     * walk cannot stop early and the mask is the whole truth about presence. */
    {
        arnm_json_field probe[FIELD_COUNT];

        for (i = 0; i != FIELD_COUNT; ++i) {
            members[i] = NULL;
            probe[i] = (arnm_json_field){kFields[i].key, (uint32_t)strlen(kFields[i].key),
                                         ARNM_JSON_FIELD_TYPE_VALUE, &members[i]};
        }
        if (arnm_json_read_object(root, probe, FIELD_COUNT, &present) ==
            ARNM_ERROR_INVALID_ENUM_TYPE) {
            /* Not an object -- and an array is not one either as far as arnm is concerned, while
             * JavaScript calls it one and finds no keys in it. So an array is answered as an
             * object with everything missing, which is what the reference path answers. */
            arnm_json_value *elements[1];
            arnm_result as_array = arnm_json_read_array(root, elements, 1, NULL);
            char rendered[64];

            if (as_array == ARNM_SUCCESS || as_array == ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL) {
                arnm_json_reader_release(&reader);
                (void)snprintf(rendered, sizeof(rendered),
                               "Invalid key: Expected \"%s\" but received undefined",
                               kFields[0].key);
                return reply_validation(req, kFields[0].key, rendered);
            }
            /* A scalar document. What it was is the body itself, which for one JSON value is
             * also what JSON.stringify would have printed. */
            {
                size_t begin = 0;
                size_t trimmed = 0;
                char message[SC_API_ERROR_MESSAGE_MAX];

                bk_trim(body, body_len, &begin, &trimmed);
                if (trimmed + 1 > sizeof(rendered))
                    trimmed = sizeof(rendered) - 1;
                memcpy(rendered, body + begin, trimmed);
                rendered[trimmed] = '\0';
                arnm_json_reader_release(&reader);
                (void)snprintf(message, sizeof(message),
                               "Invalid type: Expected Object but received %s", rendered);
                return reply_validation(req, "body", message);
            }
        }
    }

    for (i = 0; i != FIELD_COUNT; ++i) {
        arnm_memory_block text = {0};
        arnm_json_field probe[1];
        uint64_t found = 0;
        const char *refusal;
        char message[SC_API_ERROR_MESSAGE_MAX];

        if (!FOUND(present, i)) {
            arnm_json_reader_release(&reader);
            (void)snprintf(message, sizeof(message),
                           "Invalid key: Expected \"%s\" but received undefined", kFields[i].key);
            return reply_validation(req, kFields[i].key, message);
        }
        probe[0] = (arnm_json_field){kFields[i].key, (uint32_t)strlen(kFields[i].key),
                                     ARNM_JSON_FIELD_TYPE_STRING, &text};
        arnm_json_read_object(root, probe, 1, &found);
        if (!FOUND(found, 0)) {
            char rendered[64];

            render_member(root, members[i], kFields[i].key, rendered, sizeof(rendered));
            arnm_json_reader_release(&reader);
            (void)snprintf(message, sizeof(message),
                           "Invalid type: Expected string but received %s", rendered);
            return reply_validation(req, kFields[i].key, message);
        }
        refusal = apply_field_rule(&kFields[i], (const char *)text.data, text.size, values[i]);
        if (refusal != NULL) {
            arnm_json_reader_release(&reader);
            return reply_validation(req, kFields[i].key, refusal);
        }
    }

    /*
     * `language` is optional and its schema takes anything: an unknown or absent value becomes
     * the default rather than a rejected request. That is unknownValuePolicy "ignore_and_warn" in
     * contracts/types/Language.json, and it is deliberate -- the value arrives from the browser's
     * locale, which nobody typed and nobody can correct from the form.
     *
     * It is walked on its own rather than with the three above, because a member of the wrong
     * type stops an arnm walk where it stands, and a number here must not hide the fields after
     * it.
     */
    {
        arnm_memory_block text = {0};
        arnm_json_field probe[1] = {ARNM_JSON_FIELD_STRING("language", &text)};
        uint64_t found = 0;

        arnm_json_read_object(root, probe, 1, &found);
        language[0] = '\0';
        if (FOUND(found, 0) && text.data != NULL && text.size + 1 <= sizeof(language)) {
            memcpy(language, text.data, text.size);
            language[text.size] = '\0';
        }
    }

    arnm_json_reader_release(&reader);

    if (bc_register_account(context, values[0], values[1], values[2], language, error,
                            sizeof(error)) != SC_OK)
        return reply_unknown(req, "POST", error);

    /* 204, with no body at all. There is nothing a caller can do with a new account: it does not
     * exist to them until the address is confirmed, and the page's whole job afterwards is to
     * point at an inbox.
     *
     * Answering with nothing also makes the silence rule structural rather than maintained. When
     * the address is already taken there is no row to describe, so an earlier version of this
     * route on the reference path invented a gradido id and echoed the names back; two paths
     * producing an indistinguishable answer is a property somebody has to keep true. An empty
     * body is the same bytes either way, and there is no fabricated identifier for a client to
     * mistake for a real one. */
    (void)sc_http_reply(req, 204, "", "", 0);
    return 0;
}

int backend_route_not_implemented(sc_http_req *req, void *user_data)
{
    const char *path;
    size_t path_len = 0;

    (void)user_data;
    path = sc_http_path(req, &path_len);
    (void)sc_http_reply_route_not_implemented(req, path, path_len);
    return 0;
}
