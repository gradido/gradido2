#include "backend_core/uuid.h"

#include <stdint.h>

#include <sodium.h>

#include "arnm/converter.h"

void bc_new_uuid(char *out)
{
    uint8_t bytes[ARNM_UUID_BINARY_SIZE];

    if (out == NULL)
        return;
    randombytes_buf(bytes, sizeof(bytes));
    /* RFC 9562: version 4 in the high nibble of byte 6, variant 10 in the top bits of byte 8.
     * The other 122 bits stay as they came. */
    bytes[6] = (uint8_t)((bytes[6] & 0x0f) | 0x40);
    bytes[8] = (uint8_t)((bytes[8] & 0x3f) | 0x80);
    /* arnm renders it rather than a snprintf here, for the same reason jwt.c goes through its
     * json reader: one place decides what a uuid looks like on this path. */
    arnm_uuid_to_string(out, bytes);
}
