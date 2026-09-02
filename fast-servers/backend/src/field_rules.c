#include "field_rules.h"

#include <stdint.h>

/** The code point at @p index, and how many bytes it took. Malformed input yields one byte. */
static uint32_t decode(const char *text, size_t length, size_t index, size_t *width)
{
    unsigned char first = (unsigned char)text[index];
    size_t needed;
    uint32_t value;
    size_t i;

    *width = 1;
    if (first < 0x80)
        return first;
    if ((first & 0xe0) == 0xc0) {
        needed = 1;
        value = first & 0x1fu;
    } else if ((first & 0xf0) == 0xe0) {
        needed = 2;
        value = first & 0x0fu;
    } else if ((first & 0xf8) == 0xf0) {
        needed = 3;
        value = first & 0x07u;
    } else {
        return first;
    }
    /* A truncated sequence is not a code point; it counts as the one byte it is. */
    if (index + needed >= length)
        return first;
    for (i = 1; i <= needed; ++i) {
        unsigned char next = (unsigned char)text[index + i];
        if ((next & 0xc0) != 0x80)
            return first;
        value = (value << 6) | (next & 0x3fu);
    }
    *width = needed + 1;
    return value;
}

size_t bk_utf16_length(const char *text, size_t length)
{
    size_t units = 0;
    size_t i = 0;

    if (text == NULL)
        return 0;
    while (i < length) {
        size_t width = 1;
        uint32_t code = decode(text, length, i, &width);

        /* Beyond the basic plane a code point is a surrogate pair, which is two units in a
         * JavaScript string and therefore two towards a maxLength. */
        units += code > 0xffffu ? 2u : 1u;
        i += width;
    }
    return units;
}

/** ECMAScript WhiteSpace and LineTerminator. */
static int is_js_space(uint32_t code)
{
    switch (code) {
    case 0x0009:
    case 0x000a:
    case 0x000b:
    case 0x000c:
    case 0x000d:
    case 0x0020:
    case 0x00a0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202f:
    case 0x205f:
    case 0x3000:
    case 0xfeff:
        return 1;
    default:
        return code >= 0x2000 && code <= 0x200a;
    }
}

void bk_trim(const char *text, size_t length, size_t *begin, size_t *trimmed_length)
{
    size_t start = 0;
    size_t end = length;

    *begin = 0;
    *trimmed_length = 0;
    if (text == NULL)
        return;

    for (;;) {
        size_t width = 1;
        uint32_t code;

        if (start >= end)
            break;
        code = decode(text, length, start, &width);
        if (!is_js_space(code))
            break;
        start += width;
    }
    /* Backwards is a scan for the start of the last code point, which UTF-8 makes cheap: a
     * continuation byte is 10xxxxxx and nothing else is. */
    while (end > start) {
        size_t last = end - 1;
        size_t width;
        uint32_t code;

        while (last > start && ((unsigned char)text[last] & 0xc0) == 0x80)
            --last;
        code = decode(text, length, last, &width);
        if (last + width != end || !is_js_space(code))
            break;
        end = last;
    }
    *begin = start;
    *trimmed_length = end - start;
}

static int is_local_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '+' || c == '-';
}

static int is_label_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/** `[\w+-]+(?:\.[\w+-]+)*` -- labels of the local set, single dots between them. */
static int local_part_ok(const char *text, size_t length)
{
    size_t i = 0;
    size_t in_label = 0;

    if (length == 0)
        return 0;
    for (i = 0; i != length; ++i) {
        if (text[i] == '.') {
            if (in_label == 0)
                return 0;
            in_label = 0;
            continue;
        }
        if (!is_local_char(text[i]))
            return 0;
        ++in_label;
    }
    return in_label != 0;
}

/** `[\da-z]+(?:[.-][\da-z]+)*` -- labels of letters and digits, single dots or hyphens between. */
static int domain_labels_ok(const char *text, size_t length)
{
    size_t i;
    size_t in_label = 0;

    if (length == 0)
        return 0;
    for (i = 0; i != length; ++i) {
        if (text[i] == '.' || text[i] == '-') {
            if (in_label == 0)
                return 0;
            in_label = 0;
            continue;
        }
        if (!is_label_char(text[i]))
            return 0;
        ++in_label;
    }
    return in_label != 0;
}

int bk_is_email(const char *text, size_t length)
{
    size_t at = 0;
    size_t last_dot = 0;
    int has_at = 0;
    int has_dot = 0;
    size_t i;

    if (text == NULL || length == 0)
        return 0;
    for (i = 0; i != length; ++i) {
        /* The local set carries no '@', so the first one is the separator the regex means. */
        if (text[i] == '@' && !has_at) {
            at = i;
            has_at = 1;
        }
    }
    if (!has_at)
        return 0;
    if (!local_part_ok(text, at))
        return 0;

    for (i = at + 1; i != length; ++i) {
        if (text[i] == '.') {
            last_dot = i;
            has_dot = 1;
        }
    }
    if (!has_dot)
        return 0;
    /* `\.[a-z]{2,}$`, case-insensitively: two or more letters and nothing else after the dot. */
    if (length - last_dot - 1 < 2)
        return 0;
    for (i = last_dot + 1; i != length; ++i) {
        char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
            return 0;
    }
    return domain_labels_ok(text + at + 1, last_dot - at - 1);
}
