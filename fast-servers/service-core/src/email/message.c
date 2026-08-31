/*
 * The bytes of one mail. service_core/email/message.h is the specification.
 *
 * Everything here is pure: input in, buffer out, no clock read, no allocation, no I/O. That is
 * what lets the pooled mailer and the Node addon share it -- and what lets the whole file be
 * tested without a relay.
 */
#include "service_core/email/message.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * Bytes of subject one RFC 2047 encoded-word carries.
 *
 * The word itself must stay under 76 characters (RFC 2047 2: "An encoded-word may not be more
 * than 75 characters long"), and it should also leave the first line under the 78 that RFC 5322
 * recommends -- "Subject: " has already spent nine of those. 42 input bytes are 56 base64
 * characters with no padding, so a word is 10 + 56 + 2 = 68: 77 on the first line, 69 on every
 * folded one, both inside both limits.
 */
#define SC_MAIL_EW_CHUNK 42
/*
 * Room for the encoded form of the longest subject there can be.
 *
 * A chunk is at most SC_MAIL_EW_CHUNK bytes and at least three fewer, because a four-byte
 * character straddling the split pushes it back that far -- so the smaller figure bounds how
 * many words a subject can become. Each costs "=?UTF-8?B?" and "?=" around base64 of a full
 * chunk, plus the "\r\n " that joins it to the next.
 */
#define SC_MAIL_EW_WORDS (((SC_MAIL_SUBJECT_MAX - 1) / (SC_MAIL_EW_CHUNK - 3)) + 1)
#define SC_MAIL_EW_COST (10 + 4 * ((SC_MAIL_EW_CHUNK + 2) / 3) + 2 + 3)
#define SC_MAIL_SUBJECT_ENCODED_MAX (SC_MAIL_EW_WORDS * SC_MAIL_EW_COST + 1)

/* ------------------------------------------------------------------ *
 * the date
 * ------------------------------------------------------------------ */

/*
 * The day and month names of RFC 5322, spelled out rather than taken from strftime().
 *
 * %a and %b are locale dependent, and a process started under de_DE would put "Mo, 25 Aug" in a
 * header whose grammar allows exactly seven day names, all English. It would be accepted by most
 * receivers and rejected by some, which is the worst kind of bug to find.
 */
static const char *const k_day_name[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const k_month_name[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/** Writes an RFC 5322 date-time in UTC, e.g. "Mon, 25 Aug 2026 10:20:30 +0000". */
static void render_date(char *dst, size_t cap, time_t when)
{
    struct tm tm_buf;
    const struct tm *tm;
#if defined(_WIN32)
    tm = (gmtime_s(&tm_buf, &when) == 0) ? &tm_buf : NULL;
#else
    tm = gmtime_r(&when, &tm_buf);
#endif
    if (tm == NULL) {
        /* A clock this broken is not worth failing a mail over; the receiver stamps its own
         * Received: header either way. */
        snprintf(dst, cap, "Thu, 01 Jan 1970 00:00:00 +0000");
        return;
    }
    snprintf(dst, cap, "%s, %02d %s %04d %02d:%02d:%02d +0000", k_day_name[tm->tm_wday % 7],
             tm->tm_mday, k_month_name[tm->tm_mon % 12], tm->tm_year + 1900, tm->tm_hour,
             tm->tm_min, tm->tm_sec);
}

/* ------------------------------------------------------------------ *
 * the subject
 * ------------------------------------------------------------------ */

/*
 * Why a subject is not simply copied into the header.
 *
 * A header field body is US-ASCII. RFC 5322 2.2 says so, and it is not a formality: a raw UTF-8
 * subject arrives as an unlabelled byte string and what the receiver makes of it is a guess.
 * Some guess right often enough to hide the bug, some show mojibake, and a relay without
 * 8BITMIME may refuse the message. Every locale this project ships but `en` produces one.
 *
 * So a subject with a byte above 0x7F goes out as RFC 2047 encoded-words -- `=?UTF-8?B?...?=`,
 * folded onto as many lines as it takes. Base64 and not quoted-printable, although Q would be
 * shorter for German: Q has to escape '?', '_', '=', space and every special of the context the
 * word appears in, and a list like that got slightly wrong yields a subject that decodes to
 * something else rather than one that visibly fails. B has no list.
 *
 * A pure ASCII subject is left exactly as it is, which keeps the common case readable on the
 * wire and keeps all of this out of the path where it has nothing to do.
 */

static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Encodes @p n bytes into @p dst, padded, no line breaks. Returns what it wrote. */
static size_t b64_encode(char *dst, const unsigned char *src, size_t n)
{
    size_t out = 0;
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[out++] = k_b64[(v >> 18) & 0x3F];
        dst[out++] = k_b64[(v >> 12) & 0x3F];
        dst[out++] = k_b64[(v >> 6) & 0x3F];
        dst[out++] = k_b64[v & 0x3F];
    }
    if (i < n) {
        const int have_two = (i + 1 < n);
        const uint32_t v =
            ((uint32_t)src[i] << 16) | (have_two ? ((uint32_t)src[i + 1] << 8) : 0u);
        dst[out++] = k_b64[(v >> 18) & 0x3F];
        dst[out++] = k_b64[(v >> 12) & 0x3F];
        dst[out++] = have_two ? k_b64[(v >> 6) & 0x3F] : '=';
        dst[out++] = '=';
    }
    return out;
}

/*
 * How many bytes the next encoded-word takes from @p p, of the @p left that remain.
 *
 * SC_MAIL_EW_CHUNK unless that would split a character. RFC 2047 2 is explicit that an
 * encoded-word has to decode on its own -- "each ... encodes an integral number of characters"
 * -- and a receiver decoding the words separately gets a replacement character at every seam
 * otherwise. So the split walks back off any continuation byte (10xxxxxx) to the start of the
 * character it belongs to, at most three times: a longer run is not UTF-8, and input that is
 * not UTF-8 is chunked where it says rather than searched for a boundary it does not have.
 */
static size_t chunk_len(const char *p, size_t left)
{
    size_t n = SC_MAIL_EW_CHUNK;
    if (left <= n)
        return left;
    for (int back = 0; back < 3; back++) {
        if (((unsigned char)p[n] & 0xC0) != 0x80)
            break;
        n--;
    }
    /* Three steps back from SC_MAIL_EW_CHUNK cannot reach zero, so the walk always advances. */
    return n;
}

/**
 * Writes the Subject: value for @p subject into @p dst, encoded if it has to be, and reports its
 * length in @p out_len. Always NUL terminates on success; @p out_len may be NULL.
 *
 * Answers SC_ERR_MALFORMED for a subject carrying a control character. A bare CR or LF is the
 * one that matters: `Subject: %s` with a newline in the value is header injection, and everything
 * after it is a header of somebody else's choosing -- the subject being the one field here that
 * carries user data. Encoding it away would hide the attempt rather than answer it, so it is
 * refused, and so is every other C0 byte and DEL. A tab is left alone; a header may hold one.
 *
 * Answers SC_ERR_TOO_LONG for a subject past SC_MAIL_SUBJECT_MAX, and for one whose encoded form
 * would not fit @p cap. With cap = SC_MAIL_SUBJECT_ENCODED_MAX the second cannot happen -- that
 * is what the arithmetic behind the macro is for -- and the branch is there for a caller that
 * offers less.
 *
 * Not static because test_mail.cpp checks it directly; there is no header for it, because the
 * only component that may call it is this one. See the note at its declaration there.
 */
sc_status sc_mail_encode_subject(char *dst, size_t cap, const char *subject, size_t *out_len)
{
    const char *p;
    size_t left;
    size_t out = 0;
    int is_ascii = 1;

    if (dst == NULL || cap == 0 || subject == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    for (p = subject; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        if ((c < 0x20 && c != '\t') || c == 0x7F)
            return SC_ERR_MALFORMED;
        if (c >= 0x80)
            is_ascii = 0;
    }
    left = (size_t)(p - subject);
    if (left >= SC_MAIL_SUBJECT_MAX)
        return SC_ERR_TOO_LONG;

    if (is_ascii) {
        if (left + 1 > cap)
            return SC_ERR_TOO_LONG;
        memcpy(dst, subject, left + 1);
        if (out_len != NULL)
            *out_len = left;
        return SC_OK;
    }

    p = subject;
    while (left > 0) {
        const size_t take = chunk_len(p, left);
        /* "\r\n " before every word but the first, "=?UTF-8?B?" and "?=" around each, and the
         * terminator this has to leave room for. */
        const size_t need = (out > 0 ? 3u : 0u) + 10u + 4u * ((take + 2u) / 3u) + 2u + 1u;
        if (out + need > cap)
            return SC_ERR_TOO_LONG;
        if (out > 0) {
            dst[out++] = '\r';
            dst[out++] = '\n';
            dst[out++] = ' ';
        }
        memcpy(dst + out, "=?UTF-8?B?", 10);
        out += 10;
        out += b64_encode(dst + out, (const unsigned char *)p, take);
        dst[out++] = '?';
        dst[out++] = '=';
        p += take;
        left -= take;
    }
    dst[out] = '\0';
    if (out_len != NULL)
        *out_len = out;
    return SC_OK;
}

/* ------------------------------------------------------------------ *
 * the addresses and the display name
 * ------------------------------------------------------------------ */

/*
 * An address goes into a header *and* into the SMTP envelope, so a control character in it is
 * the same attack the subject is checked for: `To: %s` with a newline in the value ends the
 * header and everything after it is a header of somebody else's choosing. That it took a bare
 * LF and produced a `Bcc:` of the caller's making is not theory -- it was reproduced against a
 * test relay, which delivered the message.
 *
 * Refused rather than stripped, for the same reason the subject is: removing the newline hides
 * an attempt that somebody should see.
 *
 * Non-ASCII is left alone. An internationalised address needs SMTPUTF8 (RFC 6531) on both
 * sides, and nothing here negotiates it -- that is a feature to add, not a byte to reject.
 */
static sc_status check_address(const char *address)
{
    for (const char *p = address; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F)
            return SC_ERR_MALFORMED;
    }
    return SC_OK;
}

/*
 * Writes the display name for a From: header -- the part before the angle brackets.
 *
 * Three cases, and the middle one is the one that was wrong: a name is a *phrase* in RFC 5322
 * grammar, so `Gradido Akademie, e.V.` is not one name but two addresses as far as a parser is
 * concerned, and a raw `Förderverein` is 8-bit in a header that RFC 5322 2.2 says is US-ASCII --
 * the very thing the subject is encoded for.
 *
 *   pure ASCII, no specials    copied as it is
 *   ASCII with specials        quoted-string, with '\' and '"' escaped
 *   anything above 0x7F        RFC 2047 encoded-words, which need no quoting: base64's
 *                              alphabet is inside what RFC 2047 5(3) allows in a phrase
 */
static sc_status format_display_name(char *dst, size_t cap, const char *name)
{
    int needs_quoting = 0;
    size_t out = 1;
    size_t len;

    if (name == NULL || name[0] == '\0') {
        dst[0] = '\0';
        return SC_OK;
    }
    for (const char *p = name; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c >= 0x80)
            return sc_mail_encode_subject(dst, cap, name, NULL);
        if (c < 0x20 || c == 0x7F)
            return SC_ERR_MALFORMED;
        /* RFC 5322 3.2.3 specials, plus the backslash and the quote a quoted-string escapes. */
        if (strchr("()<>[]:;@\\,.\"", (int)c) != NULL)
            needs_quoting = 1;
    }

    len = strlen(name);
    if (!needs_quoting) {
        if (len + 1 > cap)
            return SC_ERR_TOO_LONG;
        memcpy(dst, name, len + 1);
        return SC_OK;
    }

    /* Two quotes, the terminator, and an escape for every byte in the worst case. */
    if (2 * len + 3 > cap)
        return SC_ERR_TOO_LONG;
    dst[0] = '"';
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\')
            dst[out++] = '\\';
        dst[out++] = *p;
    }
    dst[out++] = '"';
    dst[out] = '\0';
    return SC_OK;
}

/* ------------------------------------------------------------------ *
 * quoted-printable
 * ------------------------------------------------------------------ */

/*
 * Why the body is encoded at all, when it used to go out as it was.
 *
 * Two things were wrong with that, and one encoder answers both. UTF-8 is 8-bit, and a message
 * with no Content-Transfer-Encoding is 7bit by RFC 2045 6.1 -- so every mail with an umlaut in
 * it was mislabelled, and legal only over a BODY=8BITMIME transfer nobody was negotiating. And
 * a rendered document has lines of up to 3294 bytes, where RFC 5322 2.1.1 allows 998 and RFC
 * 5321 4.5.3.1.6 allows 1000 including the CRLF.
 *
 * Quoted-printable fixes both: every byte above 126 becomes =XX, so the result is 7-bit, and
 * soft line breaks keep every line under 76 characters. Base64 would do the same and would cost
 * a third more bytes on text that is mostly ASCII; QP leaves the common case readable, which
 * matters the day someone reads a message off the wire.
 *
 * It also makes the multipart boundaries below safe by construction: '=' is the one character QP
 * always escapes, so a boundary that contains one cannot appear inside an encoded part.
 */
#define SC_MAIL_QP_LINE 75 /* the 76th column is the soft break's own '=' */

static int qp_literal(unsigned char c)
{
    /* Printable ASCII except '=', which always escapes. Space and tab are decided per position,
     * see below, and CR/LF are line structure rather than content. */
    return (c >= 33 && c <= 126 && c != '=') || c == ' ' || c == '\t';
}

/*
 * The writer: one pass that counts everything and writes what fits.
 *
 * A message is laid out once, not measured once and written once -- two passes over a structure
 * this branchy is two chances to disagree. So every emitter goes through here: `used` counts
 * regardless, `dst` is written only while there is room, and the caller compares `used` against
 * the capacity afterwards. The same shape ge_run_into() uses in the renderer.
 */
typedef struct {
    char  *dst;
    size_t cap;
    size_t used;
} sc_mail_writer;

static void w_byte(sc_mail_writer *w, char c)
{
    if (w->used < w->cap)
        w->dst[w->used] = c;
    w->used++;
}

static void w_bytes(sc_mail_writer *w, const char *src, size_t n)
{
    if (w->used + n <= w->cap)
        memcpy(w->dst + w->used, src, n);
    else if (w->used < w->cap)
        memcpy(w->dst + w->used, src, w->cap - w->used);
    w->used += n;
}

static void w_str(sc_mail_writer *w, const char *s)
{
    w_bytes(w, s, strlen(s));
}

/** printf into the writer. The format is ours in every call, never the caller's. */
static void w_fmt(sc_mail_writer *w, const char *fmt, ...)
{
    va_list args;
    int     n;

    va_start(args, fmt);
    n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (n < 0)
        return;

    if (w->used + (size_t)n < w->cap) {
        va_start(args, fmt);
        (void)vsnprintf(w->dst + w->used, (size_t)n + 1, fmt, args);
        va_end(args);
    }
    w->used += (size_t)n;
}

static const char k_hex[] = "0123456789ABCDEF";

/** Writes @p text quoted-printable, CRLF line endings, no line past 76 characters. */
static void w_qp(sc_mail_writer *w, const char *text)
{
    size_t column = 0;

    for (const char *p = text; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        int                 escape;
        size_t              need;

        if (c == '\r' || c == '\n') {
            /* A hard break. Trailing whitespace before one has to be encoded, or a receiver is
             * free to strip it -- RFC 2045 6.7 (3). */
            if (w->used > 0 && column > 0) {
                const char last = w->used <= w->cap ? w->dst[w->used - 1] : '\0';
                if ((last == ' ' || last == '\t') && w->used <= w->cap) {
                    w->used--;
                    w_byte(w, '=');
                    w_byte(w, k_hex[(unsigned char)last >> 4]);
                    w_byte(w, k_hex[(unsigned char)last & 0x0F]);
                }
            }
            w_str(w, "\r\n");
            column = 0;
            if (c == '\r' && p[1] == '\n')
                p++;
            continue;
        }

        escape = !qp_literal(c);
        need = escape ? 3u : 1u;
        if (column + need > SC_MAIL_QP_LINE) {
            /* A soft break: the '=' and the CRLF are not part of the decoded text. */
            w_str(w, "=\r\n");
            column = 0;
        }
        if (escape) {
            w_byte(w, '=');
            w_byte(w, k_hex[c >> 4]);
            w_byte(w, k_hex[c & 0x0F]);
        } else {
            w_byte(w, (char)c);
        }
        column += need;
    }
    if (column > 0)
        w_str(w, "\r\n");
}

/** Writes @p n bytes base64, 76 characters to the line. */
static void w_base64(sc_mail_writer *w, const unsigned char *src, size_t n)
{
    char   line[80];
    size_t i = 0;

    while (i < n) {
        const size_t take = (n - i) >= 57 ? 57 : (n - i); /* 57 bytes -> 76 characters */
        const size_t len = b64_encode(line, src + i, take);
        w_bytes(w, line, len);
        w_str(w, "\r\n");
        i += take;
    }
}

/* ------------------------------------------------------------------ *
 * the message
 * ------------------------------------------------------------------ */

/** The domain for the Message-ID: what @p origin says, else the part of `from` after the '@'. */
static void msgid_domain(char *dst, size_t cap, const sc_mail_origin *origin)
{
    const char *at;
    if (origin->msgid_domain != NULL && origin->msgid_domain[0] != '\0') {
        snprintf(dst, cap, "%s", origin->msgid_domain);
        return;
    }
    at = origin->from != NULL ? strchr(origin->from, '@') : NULL;
    snprintf(dst, cap, "%s", (at != NULL && at[1] != '\0') ? at + 1 : "localhost");
}

/** The text/plain or text/html part, headers and body. */
static void w_text_part(sc_mail_writer *w, const char *subtype, const char *content)
{
    w_fmt(w, "Content-Type: text/%s; charset=utf-8\r\n", subtype);
    w_str(w, "Content-Transfer-Encoding: quoted-printable\r\n\r\n");
    w_qp(w, content);
}

/** One inline image: base64, and the Content-ID the HTML's `cid:` names. */
static void w_asset_part(sc_mail_writer *w, const sc_mail_asset *asset)
{
    w_fmt(w, "Content-Type: %s; name=\"%s\"\r\n",
          asset->content_type != NULL ? asset->content_type : "application/octet-stream",
          asset->filename != NULL ? asset->filename : asset->cid);
    w_str(w, "Content-Transfer-Encoding: base64\r\n");
    w_fmt(w, "Content-ID: <%s>\r\n", asset->cid);
    w_fmt(w, "Content-Disposition: inline; filename=\"%s\"\r\n\r\n",
          asset->filename != NULL ? asset->filename : asset->cid);
    w_base64(w, asset->data, asset->size);
}

/** `--boundary` or `--boundary--`, on its own line. */
static void w_boundary(sc_mail_writer *w, const char *boundary, int last)
{
    w_fmt(w, "--%s%s\r\n", boundary, last ? "--" : "");
}

/**
 * The HTML and its images: a multipart/related when there are assets, the bare HTML part when
 * there are none.
 *
 * RFC 2387: `type` names the root part, and the root is the one the others belong to -- the
 * HTML, which refers to its siblings by Content-ID.
 */
static void w_html_and_assets(sc_mail_writer *w, const sc_mail *mail, const char *boundary)
{
    if (mail->asset_count == 0) {
        w_text_part(w, "html", mail->html);
        return;
    }
    w_fmt(w, "Content-Type: multipart/related; type=\"text/html\"; boundary=\"%s\"\r\n\r\n",
          boundary);
    w_boundary(w, boundary, 0);
    w_text_part(w, "html", mail->html);
    for (uint32_t i = 0; i < mail->asset_count; i++) {
        w_str(w, "\r\n");
        w_boundary(w, boundary, 0);
        w_asset_part(w, &mail->assets[i]);
    }
    w_str(w, "\r\n");
    w_boundary(w, boundary, 1);
}

sc_status sc_mail_format(const sc_mail_origin *origin, const sc_mail *mail, uint64_t sequence,
                         int64_t now_ms, char *buffer, size_t cap, sc_mail_message *out)
{
    char date[SC_MAIL_DATE_MAX];
    char domain[SC_MAIL_ADDR_MAX];
    char subject[SC_MAIL_SUBJECT_ENCODED_MAX];
    char from_name[SC_MAIL_SUBJECT_ENCODED_MAX];
    /* '=' is in RFC 2046's bchars and is the one character quoted-printable always escapes, so
     * neither boundary can occur inside a part. The sequence and the timestamp make them unique
     * per message, and the two differ from each other. */
    char alt_boundary[80];
    char rel_boundary[80];
    sc_mail_writer w;
    sc_status status;

    if (origin == NULL || origin->from == NULL || mail == NULL || mail->to == NULL ||
        mail->subject == NULL || buffer == NULL || out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    /* One of the two, or there is nothing to send. */
    if (mail->body == NULL && mail->html == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (mail->asset_count > 0 && (mail->assets == NULL || mail->html == NULL))
        return SC_ERR_INVALID_ARGUMENT;

    /* Everything that can be refused for what it *contains* rather than for how large it is,
     * and before anything else: a mail that is not going out should not first cost a Message-ID
     * and a formatted date. All four go into headers, and a newline in any of them ends the
     * header it is in. */
    status = check_address(mail->to);
    if (status == SC_OK)
        status = check_address(origin->from);
    if (status == SC_OK)
        status = format_display_name(from_name, sizeof from_name, origin->from_name);
    if (status == SC_OK)
        status = sc_mail_encode_subject(subject, sizeof subject, mail->subject, NULL);
    if (status != SC_OK)
        return status;

    render_date(date, sizeof date, (time_t)(now_ms / 1000));
    msgid_domain(domain, sizeof domain, origin);
    snprintf(out->msgid, sizeof out->msgid, "%llu.%lld@%s", (unsigned long long)sequence,
             (long long)now_ms, domain);
    snprintf(alt_boundary, sizeof alt_boundary, "=_gradido_%llu_%lld_alt",
             (unsigned long long)sequence, (long long)now_ms);
    snprintf(rel_boundary, sizeof rel_boundary, "=_gradido_%llu_%lld_rel",
             (unsigned long long)sequence, (long long)now_ms);

    w.dst = buffer;
    w.cap = cap > 0 ? cap - 1 : 0; /* one byte kept for the terminator */
    w.used = 0;

    /* The subject may already be a folded encoded-word sequence, so it goes on its line as a
     * value that is complete rather than one to be wrapped here. */
    w_fmt(&w, "Date: %s\r\n", date);
    if (from_name[0] != '\0')
        w_fmt(&w, "From: %s <%s>\r\n", from_name, origin->from);
    else
        w_fmt(&w, "From: %s\r\n", origin->from);
    w_fmt(&w, "To: %s\r\n", mail->to);
    w_fmt(&w, "Subject: %s\r\n", subject);
    w_fmt(&w, "Message-ID: <%s>\r\n", out->msgid);
    w_str(&w, "MIME-Version: 1.0\r\n");

    if (mail->body != NULL && mail->html != NULL) {
        /* RFC 2046 5.1.4: the alternatives go in increasing order of faithfulness, so the
         * receiver can take the last one it understands. Text first, HTML second. */
        w_fmt(&w, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n\r\n", alt_boundary);
        w_boundary(&w, alt_boundary, 0);
        w_text_part(&w, "plain", mail->body);
        w_str(&w, "\r\n");
        w_boundary(&w, alt_boundary, 0);
        w_html_and_assets(&w, mail, rel_boundary);
        w_str(&w, "\r\n");
        w_boundary(&w, alt_boundary, 1);
    } else if (mail->html != NULL) {
        w_html_and_assets(&w, mail, rel_boundary);
    } else {
        w_text_part(&w, "plain", mail->body);
    }

    if (w.used + 1 > cap) {
        /* What it would have taken, so a caller with a growable buffer can ask again. */
        out->data = NULL;
        out->len = w.used + 1;
        return SC_ERR_TOO_LONG;
    }
    buffer[w.used] = '\0';
    out->data = buffer;
    out->len = w.used;
    return SC_OK;
}
