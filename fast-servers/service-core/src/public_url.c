#include "service_core/public_url.h"

#include <string.h>
#include <uv.h>

/* Room for any host the check has an answer for; a longer one is no name anybody resolves. */
#define HOST_MAX 256

static char ascii_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* The host as written, ASCII lowercased, an IPv6 literal with its brackets, into @p out. */
static int host_of(const char *url, char out[HOST_MAX])
{
    const char *scheme = strstr(url, "://");
    const char *start;
    const char *end;
    const char *at;
    size_t length;
    size_t i;

    if (scheme == NULL)
        return 0;
    start = scheme + 3;
    end = start + strcspn(start, "/?#");
    for (at = start; at != end; ++at) {
        if (*at == '@')
            start = at + 1;
    }
    if (start != end && *start == '[') {
        const char *close = memchr(start, ']', (size_t)(end - start));
        if (close == NULL)
            return 0;
        end = close + 1;
    } else {
        const char *colon = memchr(start, ':', (size_t)(end - start));
        if (colon != NULL)
            end = colon;
    }
    length = (size_t)(end - start);
    if (length == 0 || length >= HOST_MAX || (length == 2 && start[0] == '['))
        return 0;
    for (i = 0; i != length; ++i)
        out[i] = ascii_lower(start[i]);
    out[length] = '\0';
    return 1;
}

static int is_public_ipv4(const unsigned char *a)
{
    return !(a[0] == 0 || a[0] == 10 || a[0] == 127 || a[0] >= 224 ||
             (a[0] == 100 && a[1] >= 64 && a[1] <= 127) || (a[0] == 169 && a[1] == 254) ||
             (a[0] == 172 && a[1] >= 16 && a[1] <= 31) || (a[0] == 192 && a[1] == 168));
}

static int is_public_ipv6(const unsigned char *a)
{
    static const unsigned char zero[15] = {0};

    if (memcmp(a, zero, 15) == 0 && (a[15] == 0 || a[15] == 1))
        return 0;
    if (memcmp(a, zero, 10) == 0 && a[10] == 0xff && a[11] == 0xff)
        return is_public_ipv4(a + 12);
    return !((a[0] & 0xfe) == 0xfc || (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) || a[0] == 0xff);
}

static int all_of(const char *begin, const char *end, const char *set)
{
    for (; begin != end; ++begin) {
        if (strchr(set, *begin) == NULL)
            return 0;
    }
    return 1;
}

static int label_is(const char *begin, const char *end, const char *word)
{
    return (size_t)(end - begin) == strlen(word) && memcmp(begin, word, strlen(word)) == 0;
}

static int is_public_name(char *host)
{
    static const char *const kPrivate[] = {"localhost", "local", "internal",
                                           "test",      "example", "invalid"};
    size_t length = strlen(host);
    const char *last;
    const char *previous = NULL;
    size_t labels = 1;
    size_t i;

    if (length != 0 && host[length - 1] == '.')
        host[--length] = '\0';
    if (length == 0 || host[0] == '.' || host[length - 1] == '.' || strstr(host, "..") != NULL)
        return 0;
    last = host;
    for (i = 0; i != length; ++i) {
        if (host[i] == '.') {
            ++labels;
            previous = last;
            last = host + i + 1;
        }
    }
    if (labels < 2)
        return 0;
    if (all_of(last, host + length, "0123456789") ||
        (host + length - last >= 2 && last[0] == '0' && last[1] == 'x' &&
         all_of(last + 2, host + length, "0123456789abcdef")))
        return 0;
    for (i = 0; i != sizeof(kPrivate) / sizeof(kPrivate[0]); ++i) {
        if (label_is(last, host + length, kPrivate[i]))
            return 0;
    }
    return !(label_is(last, host + length, "arpa") && label_is(previous, last - 1, "home"));
}

int sc_url_is_public(const char *url)
{
    char host[HOST_MAX];
    unsigned char address[16];

    if (url == NULL || !host_of(url, host) || strchr(host, '%') != NULL)
        return 0;
    if (host[0] == '[') {
        host[strlen(host) - 1] = '\0';
        return uv_inet_pton(AF_INET6, host + 1, address) == 0 && is_public_ipv6(address);
    }
    if (uv_inet_pton(AF_INET, host, address) == 0)
        return is_public_ipv4(address);
    return is_public_name(host);
}
