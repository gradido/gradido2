/*
 * The three pieces of user logic that touch no database: the address normalization every lookup
 * agrees on, the verification code, and the gradido id. See backend_core/domain/user.h for what
 * each of them is and why.
 */
#include "backend_core/domain/user.h"

#include <stdio.h>
#include <string.h>

#include <sodium.h>

int bc_normalize_email(const char *email, char *out, size_t out_size)
{
    size_t begin = 0;
    size_t end;
    size_t i;

    if (email == NULL || out == NULL || out_size == 0)
        return 0;
    end = strlen(email);
    while (begin < end && (unsigned char)email[begin] <= ' ')
        ++begin;
    while (end > begin && (unsigned char)email[end - 1] <= ' ')
        --end;
    if (end - begin + 1 > out_size)
        return 0;
    for (i = begin; i != end; ++i) {
        char c = email[i];
        out[i - begin] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[end - begin] = '\0';
    return 1;
}

uint64_t bc_new_email_verification_code(void)
{
    /* Seven bytes is 56 bits, masked down to the low 53. A mask keeps every value equally likely,
     * which a modulo would not; the loop is only here to exclude zero. */
    for (;;) {
        uint8_t bytes[7];
        uint64_t value = 0;
        size_t i;

        randombytes_buf(bytes, sizeof(bytes));
        for (i = 0; i != sizeof(bytes); ++i)
            value = (value << 8) | bytes[i];
        value &= 0x1fffffffffffffull;
        if (value != 0)
            return value;
    }
}

/* A draw and nothing else; what makes the value unique is `users_uuid_key` at the moment of the
 * write. This exists as a named function rather than as a call to bc_new_uuid so that the reason
 * there is no lookup here has somewhere to live -- see the declaration. */
void bc_new_gradido_id(char *out)
{
    if (out == NULL)
        return;
    bc_new_uuid(out);
}
