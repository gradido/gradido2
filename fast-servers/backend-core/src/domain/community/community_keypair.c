#include "backend_core/domain/community.h"

#include <sodium.h>

void bc_community_new_keys(uint8_t public_key[BC_PUBLIC_KEY_SIZE],
                           uint8_t private_key[BC_PRIVATE_KEY_SIZE])
{
    /* crypto_sign_keypair writes exactly the two shapes the columns hold: 32 bytes of public key
     * and a 64 byte secret key that is the seed followed by the public key. The sizes are
     * asserted rather than assumed, because a libsodium built with different parameters would
     * otherwise write past a column's worth of bytes. */
    _Static_assert(crypto_sign_PUBLICKEYBYTES == BC_PUBLIC_KEY_SIZE, "ed25519 public key size");
    _Static_assert(crypto_sign_SECRETKEYBYTES == BC_PRIVATE_KEY_SIZE, "ed25519 secret key size");

    (void)crypto_sign_keypair(public_key, private_key);
}
