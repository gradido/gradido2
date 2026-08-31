/*
 * The mail client. service_core/mail.h holds the design; this file holds the parts of it a test
 * can decide.
 *
 * Most of what is worth checking is refusal and bookkeeping: an address that would have to be
 * truncated, a queue at its limit, a mail that fails twice and is written off. None of that
 * needs a relay -- a port nothing listens on refuses a connection immediately, which is a
 * failed attempt arriving in microseconds rather than after a timeout, and that is what makes
 * the retry cycle testable offline at all.
 *
 * What no test here decides is whether the rendered message is one a receiver accepts, or
 * whether the pool grows under real load. Set SC_MAIL_TEST_URL to a listening SMTP server and
 * the last tests send through it for real; without it they skip rather than pass, because a
 * green test that did nothing is worse than one that is absent.
 *
 * The concurrency is meant to be run under `-Dsanitize=thread`. EnqueueIsSafeFromManyThreads
 * proves very little on its own -- a mutex that is missing does not fail a single run -- and
 * everything under TSan.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "service_core/log.h"
#include "service_core/email/mailer.h"

/* sc_mail_encode_subject() arrives with email/message.h, which mailer.h includes. It used to be
 * declared here by hand, because the encoder had no header of its own -- the split into
 * message / transport / mailer gave it one. */
}

namespace
{

/* A relay address nothing listens on, so an attempt fails at connect rather than at a timeout. */
constexpr const char *kDeadRelay = "smtp://127.0.0.1:1";

sc_mail_config base_config()
{
    sc_mail_config config{};
    config.url = kDeadRelay;
    config.from = "bench@gradido.local";
    config.timeout_ms = 500;
    /* Short enough that a test can wait it out; the product default is half a minute. */
    config.retry_delay_ms = 50;
    return config;
}

sc_mail one_mail(const char *to = "inbox@gradido.local")
{
    sc_mail mail{};
    mail.to = to;
    mail.subject = "test";
    mail.body = "hello";
    return mail;
}

void sleep_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct MailTest : ::testing::Test {};

/* ---------------------------------------------------------------- *
 * configuration and refusal
 * ---------------------------------------------------------------- */

TEST_F(MailTest, RefusesAConfigWithoutRelayOrSender)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();

    config.url = nullptr;
    EXPECT_EQ(sc_mailer_create(&config, &mailer), SC_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(mailer, nullptr);

    config = base_config();
    config.from = nullptr;
    EXPECT_EQ(sc_mailer_create(&config, &mailer), SC_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(mailer, nullptr);
}

/* The house rule the whole module is written around: a field that does not fit is refused, not
 * shortened. A truncated relay host connects to a different machine. */
TEST_F(MailTest, RefusesAFieldThatWouldHaveToBeTruncated)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    const std::string too_long(SC_MAIL_URL_MAX + 1, 'x');
    config.url = too_long.c_str();

    EXPECT_EQ(sc_mailer_create(&config, &mailer), SC_ERR_TOO_LONG);
    EXPECT_EQ(mailer, nullptr);
}

/* And the same for a recipient, which is the one that would deliver to someone else. */
TEST_F(MailTest, RefusesARecipientThatWouldHaveToBeTruncated)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    const std::string too_long(SC_MAIL_ADDR_MAX + 1, 'x');
    sc_mail mail = one_mail(too_long.c_str());
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_ERR_TOO_LONG);
    EXPECT_EQ(sc_mail_pending(mailer), 0u);

    sc_mailer_destroy(mailer);
}

TEST_F(MailTest, EnqueueRefusesEverythingIncomplete)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    mail.to = nullptr;
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_ERR_INVALID_ARGUMENT);
    mail = one_mail();
    mail.subject = nullptr;
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_ERR_INVALID_ARGUMENT);
    mail = one_mail();
    mail.body = nullptr;
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(sc_mail_pending(mailer), 0u);

    sc_mailer_destroy(mailer);
}

/* ---------------------------------------------------------------- *
 * the subject
 * ---------------------------------------------------------------- */

/* Enough room that only the encoder's own bounds can be the reason something does not fit. */
constexpr size_t kSubjectCap = 1024;

std::string encoded(const char *subject, sc_status expect = SC_OK)
{
    std::vector<char> buf(kSubjectCap, '\xEE');
    size_t len = 0;
    EXPECT_EQ(sc_mail_encode_subject(buf.data(), buf.size(), subject, &len), expect)
        << "subject: " << subject;
    if (expect != SC_OK)
        return {};
    EXPECT_EQ(buf[len], '\0') << "not terminated";
    EXPECT_EQ(std::string(buf.data()).size(), len) << "length disagrees with the terminator";
    return std::string(buf.data(), len);
}

/* ASCII is the common case and stays readable on the wire -- an encoded-word there would be
 * correct and unhelpful. */
TEST_F(MailTest, LeavesAnAsciiSubjectAlone)
{
    EXPECT_EQ(encoded("Account activation"), "Account activation");
    EXPECT_EQ(encoded(""), "");
    /* Every printable ASCII character, including the ones RFC 2047 would have to escape. */
    EXPECT_EQ(encoded("=?UTF-8?B?_ '\"()<>@,;:\\/[]?.="), "=?UTF-8?B?_ '\"()<>@,;:\\/[]?.=");
}

/* ------------------------------------------------------------------ *
 * the headers around the subject
 * ------------------------------------------------------------------ */

/** Formats one mail and returns its header block, or "" when sc_mail_format refused it. */
std::string headers(const sc_mail_origin &origin, const sc_mail &mail, sc_status expect = SC_OK)
{
    std::vector<char> buf(64 * 1024);
    sc_mail_message   out{};
    EXPECT_EQ(sc_mail_format(&origin, &mail, 1, 1788172396000LL, buf.data(), buf.size(), &out),
              expect);
    if (expect != SC_OK)
        return {};
    const std::string message(out.data, out.len);
    return message.substr(0, message.find("\r\n\r\n"));
}

/*
 * Header injection through the recipient, which is what this check exists for.
 *
 * It was reproducible: a bare LF in `to` put a `Bcc:` of the caller's choosing into the
 * message and a test relay delivered it. The subject had been checked since the beginning;
 * the two addresses had not.
 */
TEST_F(MailTest, RefusesAControlCharacterInAnAddress)
{
    const sc_mail_origin origin{"noreply@gradido.net", nullptr, nullptr};

    sc_mail injected{"victim@example.org\nBcc: attacker@evil.test", "s", "b"};
    headers(origin, injected, SC_ERR_MALFORMED);

    sc_mail crlf{"victim@example.org\r\nBcc: attacker@evil.test", "s", "b"};
    headers(origin, crlf, SC_ERR_MALFORMED);

    const sc_mail_origin bad_sender{"noreply@gradido.net\r\nX-Spoofed: yes", nullptr, nullptr};
    sc_mail plain{"member@example.org", "s", "b"};
    headers(bad_sender, plain, SC_ERR_MALFORMED);

    /* And the honest case still goes through. */
    EXPECT_NE(headers(origin, plain).find("To: member@example.org"), std::string::npos);
}

/*
 * A display name is a phrase, not a string: an unquoted comma makes `Gradido Akademie, e.V.`
 * two addresses, and a raw umlaut is 8-bit in a header RFC 5322 2.2 says is US-ASCII -- the
 * same bug the subject was fixed for, one line further up in the same header block.
 */
TEST_F(MailTest, QuotesOrEncodesTheDisplayName)
{
    sc_mail mail{"member@example.org", "s", "b"};

    const sc_mail_origin plain{"noreply@gradido.net", "Gradido", nullptr};
    EXPECT_NE(headers(plain, mail).find("From: Gradido <noreply@gradido.net>"), std::string::npos);

    const sc_mail_origin comma{"noreply@gradido.net", "Gradido Akademie, e.V.", nullptr};
    EXPECT_NE(headers(comma, mail).find("From: \"Gradido Akademie, e.V.\" <noreply@gradido.net>"),
              std::string::npos);

    const sc_mail_origin umlaut{"noreply@gradido.net", "Gradido F\xC3\xB6rderverein", nullptr};
    EXPECT_NE(headers(umlaut, mail).find("From: =?UTF-8?B?"), std::string::npos);

    const sc_mail_origin injected{"noreply@gradido.net", "Gradido\r\nX-Spoofed: yes", nullptr};
    headers(injected, mail, SC_ERR_MALFORMED);
}

/* ------------------------------------------------------------------ *
 * the MIME structure
 * ------------------------------------------------------------------ */

/** The whole formatted message, or "" when sc_mail_format refused it. */
std::string formatted(const sc_mail_origin &origin, const sc_mail &mail,
                      sc_status expect = SC_OK)
{
    std::vector<char> buf(256 * 1024);
    sc_mail_message   out{};
    EXPECT_EQ(sc_mail_format(&origin, &mail, 1, 1788172396000LL, buf.data(), buf.size(), &out),
              expect);
    return expect == SC_OK ? std::string(out.data, out.len) : std::string();
}

/** Every line of @p message, without the CRLFs. */
std::vector<std::string> lines_of(const std::string &message)
{
    std::vector<std::string> lines;
    size_t                   pos = 0;
    for (size_t at = message.find("\r\n"); at != std::string::npos;
         at = message.find("\r\n", pos)) {
        lines.push_back(message.substr(pos, at - pos));
        pos = at + 2;
    }
    if (pos < message.size())
        lines.push_back(message.substr(pos));
    return lines;
}

const sc_mail_origin kOrigin{"noreply@gradido.net", "Gradido", nullptr};

/*
 * The two things quoted-printable is here for, and both were wrong before it: a UTF-8 body went
 * out with no Content-Transfer-Encoding at all -- 7bit by RFC 2045 6.1 -- and a rendered
 * document has lines far past the 998 RFC 5322 2.1.1 allows.
 */
TEST_F(MailTest, EncodesTheBodyQuotedPrintable)
{
    sc_mail mail{"member@example.org", "s", "Gr\xC3\xBC\xC3\x9F" "e, Bj\xC3\xB6rn", nullptr,
                 nullptr, 0};
    const std::string message = formatted(kOrigin, mail);

    EXPECT_NE(message.find("Content-Type: text/plain; charset=utf-8\r\n"), std::string::npos);
    EXPECT_NE(message.find("Content-Transfer-Encoding: quoted-printable\r\n"), std::string::npos);
    /* ü as =C3=BC, and nothing above 126 anywhere in the message. */
    EXPECT_NE(message.find("Gr=C3=BC=C3=9Fe, Bj=C3=B6rn"), std::string::npos);
    for (unsigned char c : message)
        ASSERT_LE(c, 126) << "an 8-bit byte survived the encoding";
}

TEST_F(MailTest, KeepsEveryLineWithinTheLimit)
{
    /* One line of 4000 characters, which is what a rendered document's CSS looks like. */
    const std::string long_line(4000, 'x');
    sc_mail mail{"member@example.org", "s", long_line.c_str(), nullptr, nullptr, 0};

    for (const std::string &line : lines_of(formatted(kOrigin, mail))) {
        EXPECT_LE(line.size(), 998u) << "RFC 5322 2.1.1";
        if (line.size() > 76)
            EXPECT_EQ(line.rfind("Content-", 0), 0u) << "only a header may be long: " << line;
    }
}

/* RFC 2046 5.1.4: the alternatives in increasing order of faithfulness, so a receiver takes the
 * last one it understands. */
TEST_F(MailTest, TextAndHtmlBecomeMultipartAlternative)
{
    sc_mail mail{"member@example.org", "s", "plain", "<p>rich</p>", nullptr, 0};
    const std::string message = formatted(kOrigin, mail);

    EXPECT_NE(message.find("Content-Type: multipart/alternative; boundary=\""),
              std::string::npos);
    const size_t plain = message.find("Content-Type: text/plain");
    const size_t html = message.find("Content-Type: text/html");
    ASSERT_NE(plain, std::string::npos);
    ASSERT_NE(html, std::string::npos);
    EXPECT_LT(plain, html) << "the richer alternative comes last";
    /* Opened twice, closed once. */
    EXPECT_NE(message.find("_alt--\r\n"), std::string::npos);
}

/*
 * The images the templates refer to as cid:. Until this existed the references pointed at
 * nothing and every mail arrived with six broken images.
 */
TEST_F(MailTest, HtmlWithImagesBecomesMultipartRelated)
{
    const unsigned char png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const sc_mail_asset asset{"gradidoheader", "gradido-header.png", "image/png", png,
                              sizeof png};
    sc_mail mail{"member@example.org", "s", nullptr, "<img src=\"cid:gradidoheader\">", &asset, 1};
    const std::string message = formatted(kOrigin, mail);

    EXPECT_NE(message.find("Content-Type: multipart/related; type=\"text/html\"; boundary=\""),
              std::string::npos);
    EXPECT_NE(message.find("Content-ID: <gradidoheader>\r\n"), std::string::npos);
    EXPECT_NE(message.find("Content-Transfer-Encoding: base64\r\n"), std::string::npos);
    EXPECT_NE(message.find("Content-Disposition: inline; filename=\"gradido-header.png\"\r\n"),
              std::string::npos);
    /* The PNG signature, base64 encoded. */
    EXPECT_NE(message.find("iVBORw0KGgo="), std::string::npos);
    EXPECT_NE(message.find("_rel--\r\n"), std::string::npos);
}

TEST_F(MailTest, RefusesAMailWithNeitherTextNorHtml)
{
    sc_mail nothing{"member@example.org", "s", nullptr, nullptr, nullptr, 0};
    formatted(kOrigin, nothing, SC_ERR_INVALID_ARGUMENT);

    /* An asset without HTML has nothing to be related to. */
    const sc_mail_asset asset{"cid", "f.png", "image/png", nullptr, 0};
    sc_mail orphan{"member@example.org", "s", "text", nullptr, &asset, 1};
    formatted(kOrigin, orphan, SC_ERR_INVALID_ARGUMENT);
}

/* The bug this whole path exists for: every locale but en produces one of these. */
TEST_F(MailTest, EncodesANonAsciiSubject)
{
    /* "E-Mail Überprüfung", the subject that went out as raw UTF-8. */
    EXPECT_EQ(encoded("E-Mail \xC3\x9C" "berpr\xC3\xBC" "fung"),
              "=?UTF-8?B?RS1NYWlsIMOcYmVycHLDvGZ1bmc=?=");
    /* Non-Latin scripts are the same path, and the ones where a byte string guessed as Latin-1
     * is not merely ugly but unreadable. */
    EXPECT_EQ(encoded("\xCE\x95\xCE\xBB\xCE\xBB\xCE\xAC\xCE\xB4\xCE\xB1"),
              "=?UTF-8?B?zpXOu867zqzOtM6x?=");
}

/*
 * Folding, and the two limits it is folded to: RFC 2047 gives an encoded-word 75 characters, and
 * RFC 5322 recommends 78 for the line -- of which "Subject: " has already spent nine.
 */
TEST_F(MailTest, FoldsALongSubjectIntoSeveralEncodedWords)
{
    /* 60 U+00E4, so 120 bytes: three words, and no chunk that lands on the ASCII path. */
    std::string subject;
    for (int i = 0; i < 60; i++)
        subject += "\xC3\xA4";

    const std::string out = encoded(subject.c_str());
    ASSERT_NE(out.find("\r\n "), std::string::npos) << "a subject this long was not folded";

    size_t words = 0;
    size_t line_start = 0;
    for (size_t i = 0; i <= out.size(); i++) {
        if (i != out.size() && !(out[i] == '\r' && i + 2 < out.size() && out[i + 1] == '\n'))
            continue;
        const std::string line = out.substr(line_start, i - line_start);
        /* The first line carries "Subject: " in front of it; every folded one carries a space,
         * which is already part of the separator counted here. */
        const size_t prefix = (line_start == 0) ? 9u : 1u;
        EXPECT_LE(line.size() + prefix, 78u) << "line over RFC 5322's 78: " << line;
        EXPECT_LE(line.size(), 75u) << "encoded-word over RFC 2047's 75: " << line;
        EXPECT_EQ(line.compare(0, 10, "=?UTF-8?B?"), 0) << "not an encoded-word: " << line;
        EXPECT_EQ(line.compare(line.size() - 2, 2, "?="), 0) << "unterminated word: " << line;
        words++;
        line_start = i + 3; /* past "\r\n " */
        i += 2;
    }
    EXPECT_GT(words, 1u);
}

/** Decodes one encoded-word's base64 payload. Empty on anything that is not valid base64. */
std::string b64_decode(const std::string &in)
{
    static const char *kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    if (in.empty() || in.size() % 4 != 0)
        return {};
    for (char c : in) {
        if (c == '=')
            break;
        const char *at = std::strchr(kAlphabet, c);
        if (at == nullptr || c == '\0')
            return {};
        acc = (acc << 6) | static_cast<uint32_t>(at - kAlphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return out;
}

/** Splits an encoded subject at its folds and returns what each word decodes to. */
std::vector<std::string> decode_words(const std::string &out)
{
    std::vector<std::string> words;
    for (size_t at = 0; at < out.size();) {
        const size_t fold = out.find("\r\n ", at);
        const std::string word = out.substr(at, fold == std::string::npos ? fold : fold - at);
        EXPECT_EQ(word.compare(0, 10, "=?UTF-8?B?"), 0) << word;
        EXPECT_EQ(word.compare(word.size() - 2, 2, "?="), 0) << word;
        words.push_back(b64_decode(word.substr(10, word.size() - 12)));
        if (fold == std::string::npos)
            break;
        at = fold + 3;
    }
    return words;
}

/** True when @p s is a run of complete UTF-8 sequences -- none started before it, none cut off. */
bool whole_characters(const std::string &s)
{
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t width = (c < 0x80) ? 1u : (c >> 5) == 0x06 ? 2u : (c >> 4) == 0x0E ? 3u
                             : (c >> 3) == 0x1E                 ? 4u
                                                                : 0u;
        if (width == 0 || i + width > s.size())
            return false; /* a continuation byte first, or a sequence running past the end */
        for (size_t k = 1; k < width; k++)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return false;
        i += width;
    }
    return true;
}

/*
 * RFC 2047 2: an encoded-word "encodes an integral number of characters". A receiver decodes the
 * words one at a time, so a character split across two of them is a replacement character in
 * somebody's inbox -- which is exactly what a fixed 42-byte chunk would produce, 42 being no
 * multiple of 3 or 4.
 *
 * Padding inside a word, on the other hand, is not a symptom of anything: a chunk shortened to
 * land on a character boundary is often no multiple of three, and RFC 2047 asks each word to
 * decode on its own, which is what the padding is for.
 */
TEST_F(MailTest, NeverSplitsACharacterAcrossTwoEncodedWords)
{
    /* Four-byte characters, the worst case: the split can land on any of three continuation
     * bytes, so the padding walks it over every offset it can take. */
    for (int pad = 0; pad < 8; pad++) {
        std::string subject(static_cast<size_t>(pad), 'a');
        for (int i = 0; i < 20; i++)
            subject += "\xF0\x9F\x92\xB6"; /* U+1F4B6 */

        const std::vector<std::string> words = decode_words(encoded(subject.c_str()));
        ASSERT_GT(words.size(), 1u) << "at pad " << pad << ": nothing was folded";

        std::string joined;
        for (const std::string &word : words) {
            EXPECT_TRUE(whole_characters(word)) << "word cut mid-character, at pad " << pad;
            joined += word;
        }
        /* And decoding them one at a time gives the subject back, byte for byte. */
        EXPECT_EQ(joined, subject) << "at pad " << pad;
    }
}

/* The same round trip over the three shorter widths and over a subject that mixes them. */
TEST_F(MailTest, EncodedWordsDecodeBackToTheSubject)
{
    const char *const cases[] = {
        "E-Mail \xC3\x9C" "berpr\xC3\xBC" "fung",
        "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xAC\xCE\xB4\xCE\xB1 \xD0\x9F\xD1\x80\xD0\xBE\xD0\xB2",
        "a \xC3\xA4 \xE2\x82\xAC \xF0\x9F\x92\xB6 and back to ascii",
    };
    for (const char *subject : cases) {
        std::string joined;
        for (const std::string &word : decode_words(encoded(subject)))
            joined += word;
        EXPECT_EQ(joined, std::string(subject));
    }

    /* Long enough to fold, and every width crossing the splits. */
    std::string mixed;
    while (mixed.size() < SC_MAIL_SUBJECT_MAX - 8)
        mixed += "a\xC3\xA4\xE2\x82\xAC\xF0\x9F\x92\xB6";
    std::string joined;
    for (const std::string &word : decode_words(encoded(mixed.c_str())))
        joined += word;
    EXPECT_EQ(joined, mixed);
}

/*
 * The subject is the field here most likely to carry user data, and `Subject: %s` with a newline
 * in the value is header injection -- everything after it a header of somebody else's choosing.
 * Encoding it away would hide the attempt; refusing it answers it.
 */
TEST_F(MailTest, RefusesASubjectThatWouldInjectAHeader)
{
    encoded("hello\r\nBcc: someone@elsewhere.invalid", SC_ERR_MALFORMED);
    encoded("hello\nBcc: someone@elsewhere.invalid", SC_ERR_MALFORMED);
    encoded("hello\rthere", SC_ERR_MALFORMED);
    /* And the rest of C0, plus DEL. A tab is not among them: a header may hold one. */
    encoded("hello\x01there", SC_ERR_MALFORMED);
    encoded("hello\x7Fthere", SC_ERR_MALFORMED);
    EXPECT_EQ(encoded("hello\tthere"), "hello\tthere");
}

/* The house rule again, this time on the one field that is measured before it is encoded: the
 * bound is on the subject the caller passed, not on what it becomes. */
TEST_F(MailTest, RefusesASubjectThatWouldHaveToBeTruncated)
{
    const std::string too_long(SC_MAIL_SUBJECT_MAX, 'x');
    encoded(too_long.c_str(), SC_ERR_TOO_LONG);

    const std::string fits(SC_MAIL_SUBJECT_MAX - 1, 'x');
    EXPECT_EQ(encoded(fits.c_str()).size(), fits.size());

    /* And a caller offering less room than the encoded form needs is told so rather than handed
     * a subject that stops halfway. */
    std::vector<char> small(32, '\xEE');
    EXPECT_EQ(sc_mail_encode_subject(small.data(), small.size(), "\xC3\x9C" " berpr\xC3\xBC" "fung "
                                                                "\xC3\x9C" " berpr\xC3\xBC" "fung",
                                     nullptr),
              SC_ERR_TOO_LONG);
}

/* What the mailer does with all of the above: the refusals reach the caller of enqueue, and
 * nothing lands on the queue. */
TEST_F(MailTest, EnqueueRefusesASubjectAHeaderCannotCarry)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    mail.subject = "hello\r\nBcc: someone@elsewhere.invalid";
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_ERR_MALFORMED);

    const std::string too_long(SC_MAIL_SUBJECT_MAX, 'x');
    mail = one_mail();
    mail.subject = too_long.c_str();
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_ERR_TOO_LONG);
    EXPECT_EQ(sc_mail_pending(mailer), 0u);

    /* The arena the refused mails borrowed went back, so the queue is still worth its capacity. */
    mail = one_mail();
    mail.subject = "E-Mail \xC3\x9C" "berpr\xC3\xBC" "fung";
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);
    EXPECT_EQ(sc_mail_pending(mailer), 1u);

    sc_mailer_destroy(mailer);
}

/* ---------------------------------------------------------------- *
 * the queue and its ceiling
 * ---------------------------------------------------------------- */

TEST_F(MailTest, EnqueueRendersWithoutDialling)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);
    /* Two mails are waiting and the relay above does not exist, which it would have had to if
     * enqueue dialled. */
    EXPECT_EQ(sc_mail_pending(mailer), 2u);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.queued, 2u);
    EXPECT_EQ(stats.sent, 0u);

    sc_mailer_destroy(mailer);
}

TEST_F(MailTest, AFullQueueSaysSoRatherThanGrowing)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.queue_max = 4;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK) << "at " << i;
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_ERR_QUEUE_FULL);
    EXPECT_EQ(sc_mail_pending(mailer), 4u);

    sc_mailer_destroy(mailer);
}

/* A message larger than one arena is refused, and must not leave a half-built entry or a
 * borrowed arena behind -- which the second half of this test is what catches. */
TEST_F(MailTest, AMessageLargerThanItsArenaIsRefusedCleanly)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.queue_max = 2;
    config.message_max = 2048;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    const std::string huge(4096, 'x');
    sc_mail big = one_mail();
    big.body = huge.c_str();
    EXPECT_EQ(sc_mail_enqueue(mailer, &big), SC_ERR_TOO_LONG);
    EXPECT_EQ(sc_mail_pending(mailer), 0u);

    /* Both arenas are still there to be borrowed, so the refusal gave its one back. */
    sc_mail small = one_mail();
    EXPECT_EQ(sc_mail_enqueue(mailer, &small), SC_OK);
    EXPECT_EQ(sc_mail_enqueue(mailer, &small), SC_OK);

    sc_mailer_destroy(mailer);
}

/* ---------------------------------------------------------------- *
 * retry
 * ---------------------------------------------------------------- */

/*
 * The whole retry cycle, without a network: an attempt that fails puts the mail back rather than
 * counting it, the mail is not tried again before the delay, and the second failure is the end
 * of it.
 */
TEST_F(MailTest, AFailedMailIsTriedOnceMoreAfterThePauseAndThenGivenUp)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.retry_delay_ms = 200;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);

    /* First attempt. It failed, but it is not a failure yet -- it is on the retry ring. */
    uint32_t sent = 99;
    uint32_t failed = 99;
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_ERR_NETWORK);
    EXPECT_EQ(sent, 0u);
    EXPECT_EQ(failed, 0u);
    EXPECT_EQ(sc_mail_pending(mailer), 1u);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.retried, 1u);
    EXPECT_EQ(stats.failed, 0u);

    /* Too early: the pause has not passed, so this flush finds nothing due and touches nothing. */
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_OK);
    EXPECT_EQ(sent, 0u);
    EXPECT_EQ(failed, 0u);
    EXPECT_EQ(sc_mail_pending(mailer), 1u);

    sleep_ms(300);

    /* Now it is due, it fails again, and that is the end of it. */
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_ERR_NETWORK);
    EXPECT_EQ(sent, 0u);
    EXPECT_EQ(failed, 1u);
    EXPECT_EQ(sc_mail_pending(mailer), 0u);

    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.retried, 1u);
    EXPECT_EQ(stats.failed, 1u);
    EXPECT_EQ(stats.sent, 0u);

    sc_mailer_destroy(mailer);
}

/* There is no third attempt. A mail given up on is gone, and the arena it held came back --
 * which the refill proves. */
TEST_F(MailTest, ThereIsNoThirdAttempt)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.queue_max = 1;
    config.retry_delay_ms = 50;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);
    (void)sc_mail_flush(mailer, nullptr, nullptr);
    sleep_ms(120);
    (void)sc_mail_flush(mailer, nullptr, nullptr);
    ASSERT_EQ(sc_mail_pending(mailer), 0u);

    sleep_ms(120);
    uint32_t sent = 99;
    uint32_t failed = 99;
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_OK);
    EXPECT_EQ(sent, 0u);
    EXPECT_EQ(failed, 0u);

    /* The one arena the pool has is free again, so a new mail fits. */
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);

    sc_mailer_destroy(mailer);
}

TEST_F(MailTest, FlushingAnEmptyQueueIsNotAFailure)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    uint32_t sent = 99;
    uint32_t failed = 99;
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_OK);
    EXPECT_EQ(sent, 0u);
    EXPECT_EQ(failed, 0u);
    EXPECT_EQ(sc_mail_drain(mailer, 10), SC_OK);

    sc_mailer_destroy(mailer);
}

/* ---------------------------------------------------------------- *
 * workers
 * ---------------------------------------------------------------- */

TEST_F(MailTest, WorkersStartAndAreJoinedAgain)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.workers = 2;
    config.worker_max = 4;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.workers, 2u);

    /* The interesting half is that this returns at all: a worker sleeping on a condition
     * variable has to be woken and joined, not left behind. */
    sc_mailer_destroy(mailer);
}

/* workers = 0 and workers > 0 are two modes, and the one function that only belongs to the first
 * says so instead of racing the workers for the queue. */
TEST_F(MailTest, FlushBelongsToTheCallerOnlyWhenNoWorkerRuns)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.workers = 1;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    EXPECT_EQ(sc_mail_flush(mailer, nullptr, nullptr), SC_ERR_UNAVAILABLE);

    sc_mailer_destroy(mailer);
}

/* A worker takes the queue down on its own, and the mails end as failures rather than staying
 * put -- against this relay every attempt fails, so the two attempts and the pause are the whole
 * lifetime of each mail. */
TEST_F(MailTest, AWorkerEmptiesTheQueueWithoutBeingAsked)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.workers = 1;
    config.retry_delay_ms = 50;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    for (int i = 0; i < 5; i++)
        ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);

    EXPECT_EQ(sc_mail_drain(mailer, 5000), SC_OK);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.queued, 5u);
    EXPECT_EQ(stats.retried, 5u);
    EXPECT_EQ(stats.failed, 5u);
    EXPECT_EQ(stats.pending, 0u);

    sc_mailer_destroy(mailer);
}

/* Enqueue is called from wherever a request happens to be running, so it has to be safe from
 * several threads at once. Under TSan this is the test that matters; without it, it mostly
 * proves that nothing crashes. */
TEST_F(MailTest, EnqueueIsSafeFromManyThreads)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.workers = 2;
    config.queue_max = 512;
    config.retry_delay_ms = 10;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&mailer, &accepted, t] {
            const std::string to = "user" + std::to_string(t) + "@gradido.local";
            for (int i = 0; i < 20; i++) {
                sc_mail mail = one_mail(to.c_str());
                if (sc_mail_enqueue(mailer, &mail) == SC_OK)
                    accepted.fetch_add(1);
            }
        });
    }
    for (auto &thread : threads)
        thread.join();

    EXPECT_EQ(sc_mail_drain(mailer, 10000), SC_OK);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    /* Every mail that was accepted is accounted for exactly once, whichever way it ended. */
    EXPECT_EQ(stats.queued, static_cast<uint64_t>(accepted.load()));
    EXPECT_EQ(stats.sent + stats.failed, static_cast<uint64_t>(accepted.load()));
    EXPECT_EQ(stats.pending, 0u);

    sc_mailer_destroy(mailer);
}

/* Destroying a mailer with mails still queued must not hang and must not leak the arenas they
 * hold -- the second is what Valgrind and ASan see; this only makes it happen. */
TEST_F(MailTest, DestroyWithAFullQueueIsClean)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.queue_max = 16;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    for (int i = 0; i < 16; i++)
        ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);

    sc_mailer_destroy(mailer);
}

TEST_F(MailTest, NullIsAccepted)
{
    sc_mail_stats stats{};
    EXPECT_EQ(sc_mail_pending(nullptr), 0u);
    sc_mail_get_stats(nullptr, &stats);
    EXPECT_EQ(stats.queued, 0u);
    sc_mailer_destroy(nullptr);
    EXPECT_EQ(sc_mail_flush(nullptr, nullptr, nullptr), SC_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(sc_mail_drain(nullptr, 1), SC_ERR_INVALID_ARGUMENT);
}

/* ---------------------------------------------------------------- *
 * with someone on the other end
 * ---------------------------------------------------------------- */

/*
 * Skipped unless SC_MAIL_TEST_URL names a listening SMTP server, because a green test that sent
 * nothing says nothing.
 *
 *   ../h20Test/smtp_client/sink.py --port 2525 &
 *   SC_MAIL_TEST_URL=smtp://127.0.0.1:2525 zig-out/bin/test_mail
 */
const char *live_url()
{
    return std::getenv("SC_MAIL_TEST_URL");
}

TEST_F(MailTest, SendsThroughARealRelay)
{
    const char *url = live_url();
    if (url == nullptr)
        GTEST_SKIP() << "set SC_MAIL_TEST_URL to a listening SMTP server";

    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.url = url;
    config.from_name = "Gradido";
    config.timeout_ms = 5000;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    /* A body with every line ending this thing is meant to normalise, and a line that starts
     * with a dot -- which curl stuffs and this code must not. The subject is the one that went
     * out as raw UTF-8: whether a receiver accepts the encoded form is the question only a real
     * relay can answer, and it is why this test exists. */
    sc_mail mail = one_mail();
    mail.subject = "service-core: E-Mail \xC3\x9C" "berpr\xC3\xBC" "fung";
    mail.body = "first line\nsecond line\r\nthird line\r.leading dot\nlast";

    uint32_t sent = 0;
    uint32_t failed = 0;
    ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_OK);
    EXPECT_EQ(sent, 1u);
    EXPECT_EQ(failed, 0u);

    /* The second mail is the one that proves the session was kept: it rides the connection the
     * first one opened. Its subject is long enough to fold, which is the case worth putting on a
     * real wire -- a folded header is the only thing this module writes that contains a CRLF of
     * its own, and curl's dot-stuffing reader is watching for exactly that sequence. */
    mail.subject = "service-core: eine \xC3\x9C" "berschrift, die lang genug ist, um "
                   "\xC3\xBC" "ber mehrere Zeilen gefaltet zu werden, mit Umlauten "
                   "\xC3\xA4\xC3\xB6\xC3\xBC und einem \xE2\x82\xAC";
    ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_OK);
    EXPECT_EQ(sent, 1u);

    sc_mailer_destroy(mailer);
}

/* The same, driven by workers instead of by the caller: what a server actually does. */
TEST_F(MailTest, WorkersSendThroughARealRelay)
{
    const char *url = live_url();
    if (url == nullptr)
        GTEST_SKIP() << "set SC_MAIL_TEST_URL to a listening SMTP server";

    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.url = url;
    config.timeout_ms = 5000;
    config.workers = 1;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    mail.subject = "service-core worker test";
    for (int i = 0; i < 20; i++)
        ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK) << "at " << i;

    EXPECT_EQ(sc_mail_drain(mailer, 20000), SC_OK);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.sent, 20u);
    EXPECT_EQ(stats.failed, 0u);
    EXPECT_EQ(stats.retried, 0u);

    sc_mailer_destroy(mailer);
}

/*
 * The pool growing under load, which is the one behaviour no offline test can show: it needs a
 * relay slow enough that a worker stays busy past spawn_after_ms with a backlog behind it.
 *
 *   scratch/slowsink.py 2550 0.2 &
 *   SC_MAIL_SLOW_URL=smtp://127.0.0.1:2550 zig-out/bin/test_mail --gtest_filter=*ScaleUp*
 *
 * Any SMTP server that answers slowly will do; 150 ms per mail or more is enough.
 */
TEST_F(MailTest, WorkersScaleUpUnderLoadAndRetireAfterwards)
{
    const char *url = std::getenv("SC_MAIL_SLOW_URL");
    if (url == nullptr)
        GTEST_SKIP() << "set SC_MAIL_SLOW_URL to a deliberately slow SMTP server";

    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.url = url;
    config.timeout_ms = 30000;
    config.workers = 1;
    config.worker_max = 8;
    config.queue_max = 128;
    /* Small enough that a test does not take a minute, and the same shape as the defaults. */
    config.spawn_after_ms = 300;
    config.spawn_backlog = 4;
    config.linger_ms = 500;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    ASSERT_EQ(stats.workers, 1u);

    sc_mail mail = one_mail();
    mail.subject = "service-core scale test";
    for (int i = 0; i < 60; i++)
        ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK) << "at " << i;

    /* The burst arrives in microseconds, so nobody has been busy long enough yet -- the growth
     * has to come from the workers noticing it themselves while they work. */
    uint32_t peak = 0;
    for (int i = 0; i < 100 && peak < 2u; i++) {
        sleep_ms(100);
        sc_mail_get_stats(mailer, &stats);
        if (stats.workers > peak)
            peak = stats.workers;
        if (stats.pending == 0)
            break;
    }
    EXPECT_GT(peak, 1u) << "the pool never grew past its permanent worker";
    EXPECT_LE(peak, 8u) << "the pool went past worker_max";

    EXPECT_EQ(sc_mail_drain(mailer, 60000), SC_OK);
    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.sent, 60u);
    EXPECT_EQ(stats.failed, 0u);

    /* And they go away again: linger_ms after the queue ran dry, only the permanent one is left.
     * A pool that grows and never shrinks holds connections a relay counts against us. */
    for (int i = 0; i < 60; i++) {
        sc_mail_get_stats(mailer, &stats);
        if (stats.workers == 1u)
            break;
        sleep_ms(100);
    }
    EXPECT_EQ(stats.workers, 1u) << "the on-demand workers never retired";

    sc_mailer_destroy(mailer);
}

} // namespace

int main(int argc, char **argv)
{
    /* Quiet: the failure paths below log a line each, and a hundred of them would bury the one
     * line that says which assertion failed. SC_MAIL_TEST_LOG turns them back on when a test is
     * being worked on. */
    sc_log_init(std::getenv("SC_MAIL_TEST_LOG") != nullptr ? SC_LOG_DEBUG : SC_LOG_FATAL);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
