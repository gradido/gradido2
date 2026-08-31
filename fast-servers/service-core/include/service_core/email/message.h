/*
 * One mail as bytes: the RFC 5322 message a relay is handed.
 *
 * No sockets, no threads, no queue -- this half is what both callers share. service-core's own
 * mailer (email/mailer.h) formats into a pooled arena and hands the result to a worker; the
 * Node addon in packages/email-native formats into a buffer of its own and sends on a libuv
 * thread pool thread. Both produce the same bytes, and that is the point of the file: the
 * subject encoding, the CRLF rewrite and the header block are decided once.
 *
 * What is deliberately *not* here: dot stuffing and the terminating ".\r\n". curl's SMTP
 * reader does both -- lib/smtp.c, cr_eob_read() -- and a message that arrived stuffed would
 * carry two dots where it meant one.
 */
#ifndef SERVICE_CORE_EMAIL_MESSAGE_H
#define SERVICE_CORE_EMAIL_MESSAGE_H

#include <stddef.h>
#include <stdint.h>

#include "service_core/status.h"

/*
 * RFC 5321 4.5.3.1.3: a forward-path is at most 256 octets including the angle brackets. The
 * address inside them is therefore 254, and one more byte carries the terminator.
 */
#define SC_MAIL_ADDR_MAX 255
/*
 * Bytes of subject, terminator included, and the same ceiling for the sender's display name.
 * A longer one is refused rather than shortened -- see sc_mail.subject, which is also where the
 * encoding this bound is measured before happens.
 */
#define SC_MAIL_SUBJECT_MAX 256
/* smtp://host:port, smtps://host:port */

/* A Date: header, "Mon, 25 Aug 2026 10:20:30 +0000", plus room. */
#define SC_MAIL_DATE_MAX 48
/* "<sequence.unixms@domain>", the angle brackets excluded. */
#define SC_MAIL_MSGID_MAX (64 + SC_MAIL_ADDR_MAX)

/*
 * One inline image, as the HTML refers to it. Borrowed for the call, like everything else here.
 *
 * The same shape the renderer's ge_asset_t has, and deliberately a type of its own: a message is
 * not a thing that knows about templates, and email/render.h does not travel with this header.
 */
typedef struct sc_mail_asset {
    /* The `cid:` value, without the angle brackets. */
    const char *cid;
    const char *filename;
    /* "image/png" and the like. */
    const char *content_type;
    const unsigned char *data;
    size_t size;
} sc_mail_asset;

/* One mail, as the caller has it. Nothing here is kept: enqueue renders it and copies what it
 * needs, so all three may point into the caller's stack. */
typedef struct sc_mail {
    /* The single recipient. */
    const char *to;
    /*
     * Plain UTF-8, at most SC_MAIL_SUBJECT_MAX - 1 bytes.
     *
     * A header field body is US-ASCII -- RFC 5322 2.2 -- so a subject that is not gets encoded
     * on the way out, as RFC 2047 base64 encoded-words folded across as many lines as it takes.
     * That is not cosmetic: raw UTF-8 in a header reaches the receiver unlabelled and is
     * displayed on a guess, and a relay without 8BITMIME may refuse the message. Every locale
     * this project ships but `en` produces such a subject. One that is pure ASCII is passed
     * through untouched.
     *
     * A subject carrying a control character is refused with SC_ERR_MALFORMED. The one that
     * matters is a bare CR or LF: in a header that is injection, everything after it becoming a
     * header of somebody else's choosing, and the subject is the field here most likely to
     * carry user data. Encoding it away would hide the attempt instead of answering it.
     */
    const char *subject;
    /*
     * The plain text alternative, UTF-8, NUL terminated, with whatever line endings the caller
     * has: a bare LF becomes the CRLF RFC 5322 wants. NULL when there is only HTML.
     *
     * Dot stuffing is deliberately *not* done here. curl's SMTP reader does it -- lib/smtp.c,
     * cr_eob_read() watches for "\r\n." and inserts the second dot, adjusting infilesize as it
     * goes -- and a message stuffed on the way in would arrive with two dots where it meant
     * one. It also appends the terminating "\r\n.\r\n", so the rendered message does not carry
     * one.
     */
    const char *body;
    /* The HTML alternative, UTF-8. NULL when there is only text. At least one of the two has
     * to be there. */
    const char *html;
    /*
     * The images the HTML refers to as `cid:<cid>`. Attached as a multipart/related sibling of
     * the HTML part, base64, `Content-Disposition: inline`.
     *
     * Only reachable from the HTML: an asset nothing references is still sent, and a `cid:`
     * with no asset is a broken image -- which is what every mail this project sent looked like
     * until the parts below existed.
     */
    const sc_mail_asset *assets;
    uint32_t asset_count;
} sc_mail;

/* Who the mail is from, as the headers say it. Every string is borrowed for the call. */
typedef struct sc_mail_origin {
    /* Required. The envelope sender, and the From: address. Refused with SC_ERR_MALFORMED for a
     * control character; non-ASCII is passed through, which needs SMTPUTF8 to be of any use. */
    const char *from;
    /* Optional display name. NULL or empty sends the bare address; anything else is quoted or
     * encoded as the header grammar requires. */
    const char *from_name;
    /* The domain for the Message-ID. NULL or empty takes the part of `from` after the '@', and
     * "localhost" when there is none. */
    const char *msgid_domain;
} sc_mail_origin;

/* Where sc_mail_format() left the message. `data` points into the caller's buffer. */
typedef struct sc_mail_message {
    const char *data;
    size_t len;
    /* Assigned by the format call and worth keeping: a mail delivered twice under two identities
     * is a duplicate in someone's inbox, so a retry has to reuse this one. */
    char msgid[SC_MAIL_MSGID_MAX];
} sc_mail_message;

/**
 * Formats @p mail into @p buffer as an RFC 5322 message, NUL terminated, and describes it in
 * @p out.
 *
 * @p sequence and @p now_ms make the Message-ID; @p now_ms is also the Date: header, so the two
 * cannot disagree. The caller passes its own clock rather than one being read here -- the mailer
 * uses the log's, so a log line and the mail it describes carry the same instant.
 *
 * Two passes: the header block is measured with snprintf against a zero-length buffer, the body
 * by walking it, and only then is anything written. SC_ERR_TOO_LONG when the result would not
 * fit @p cap, and then @p out->len carries what it would have taken -- the caller may grow and
 * ask again.
 *
 * SC_ERR_MALFORMED for a control character in the subject, in either address or in the display
 * name. Each of the four goes into a header, and a bare CR or LF in one ends that header --
 * everything after it becoming a header of somebody else's choosing. Refused rather than
 * stripped: removing it would hide an attempt somebody should see.
 *
 * The display name is put on the wire the way RFC 5322 wants it: quoted when it carries a
 * special (a comma in `Gradido Akademie, e.V.` otherwise makes it two addresses), RFC 2047
 * encoded when it is not ASCII.
 */
sc_status sc_mail_format(const sc_mail_origin *origin, const sc_mail *mail, uint64_t sequence,
                         int64_t now_ms, char *buffer, size_t cap, sc_mail_message *out);

/**
 * Writes the Subject: value for @p subject into @p dst, RFC 2047 encoded if it has to be, and
 * reports its length in @p out_len (which may be NULL). Always NUL terminates on success.
 *
 * Public because sc_mail_format() is not the only thing that has to be able to check it, and
 * because test_mail.cpp exercises it directly.
 */
sc_status sc_mail_encode_subject(char *dst, size_t cap, const char *subject, size_t *out_len);

#endif /* SERVICE_CORE_EMAIL_MESSAGE_H */
