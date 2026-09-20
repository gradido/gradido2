#include "peer_id.h"

#include <string.h>

static const char kAlphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
static const uint8_t kPrefix[6] = {0x00, 0x24, 0x08, 0x01, 0x12, 0x20};

#define DHT_PEER_ID_BYTES 38

void dht_peer_id_format(const uint8_t key[32], char out[DHT_PEER_ID_TEXT_MAX])
{
    uint8_t bytes[DHT_PEER_ID_BYTES];
    uint8_t digits[DHT_PEER_ID_TEXT_MAX];
    size_t count = 0;
    size_t zeros = 0;
    size_t i;
    size_t j;

    memcpy(bytes, kPrefix, sizeof(kPrefix));
    memcpy(bytes + sizeof(kPrefix), key, 32);
    while (zeros != sizeof(bytes) && bytes[zeros] == 0)
        ++zeros;
    /* Base conversion, 256 to 58, least significant digit first. */
    for (i = zeros; i != sizeof(bytes); ++i) {
        unsigned carry = bytes[i];
        for (j = 0; j != count; ++j) {
            carry += (unsigned)digits[j] << 8;
            digits[j] = (uint8_t)(carry % 58);
            carry /= 58;
        }
        while (carry != 0) {
            digits[count++] = (uint8_t)(carry % 58);
            carry /= 58;
        }
    }
    for (i = 0; i != zeros; ++i)
        out[i] = '1';
    for (j = 0; j != count; ++j)
        out[zeros + j] = kAlphabet[digits[count - 1 - j]];
    out[zeros + count] = '\0';
}

int dht_peer_id_parse(const char *text, size_t len, uint8_t key[32])
{
    uint8_t bytes[DHT_PEER_ID_BYTES];
    size_t count = 0;
    size_t zeros = 0;
    size_t i;
    size_t j;

    if (len == 0 || len >= DHT_PEER_ID_TEXT_MAX)
        return 0;
    while (zeros != len && text[zeros] == '1')
        ++zeros;
    for (i = zeros; i != len; ++i) {
        const char *at = memchr(kAlphabet, text[i], sizeof(kAlphabet) - 1);
        unsigned carry;

        if (at == NULL || text[i] == '\0')
            return 0;
        carry = (unsigned)(at - kAlphabet);
        /* bytes is filled least significant first, like the digits above. */
        for (j = 0; j != count; ++j) {
            carry += (unsigned)bytes[j] * 58;
            bytes[j] = (uint8_t)carry;
            carry >>= 8;
        }
        while (carry != 0) {
            if (count == sizeof(bytes))
                return 0;
            bytes[count++] = (uint8_t)carry;
            carry >>= 8;
        }
    }
    if (zeros + count != DHT_PEER_ID_BYTES)
        return 0;
    /* The leading zeros, then the rest reversed into place. */
    {
        uint8_t id[DHT_PEER_ID_BYTES];
        memset(id, 0, zeros);
        for (j = 0; j != count; ++j)
            id[zeros + j] = bytes[count - 1 - j];
        if (memcmp(id, kPrefix, sizeof(kPrefix)) != 0)
            return 0;
        memcpy(key, id + sizeof(kPrefix), 32);
    }
    return 1;
}
