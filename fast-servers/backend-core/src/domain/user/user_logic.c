/*
 * The three pieces of user logic that touch no database: the address normalization every lookup
 * agrees on, the verification code, and the gradido id ladder. See backend_core/domain/user.h
 * for what each of them is and why.
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

sc_status bc_new_gradido_id(int (*exists)(const char *gradido_id, void *user_data), void *user_data,
                            char *out)
{
    int draw;

    if (exists == NULL || out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    for (draw = 0; draw != BC_GRADIDO_ID_MAX_DRAWS; ++draw) {
        int taken;

        bc_new_uuid(out);
        taken = exists(out, user_data);
        if (taken < 0)
            return SC_ERR_INVALID_ARGUMENT;
        if (taken == 0)
            return SC_OK;
    }
    out[0] = '\0';
    return SC_ERR_UNAVAILABLE;
}
