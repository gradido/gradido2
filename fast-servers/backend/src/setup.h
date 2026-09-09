/*
 * The setup command's conversation: what this installation is, and where it sends its mail.
 *
 * Reached from backend_setup() and from nowhere else; a serving start does not come here at
 * all, see bc_context_open. `packages/backend/src/setup/askForSetup.ts` is the reference this
 * is held to -- the same questions in the same order, and the same block proposed for a
 * development installation.
 *
 * Two kinds of answer come out of it and they are kept apart on purpose. The community's name,
 * description and URL are its **identity** and become a row -- written once, referred to
 * afterwards, and carrying a key pair that could never have come from an env file. The
 * database and the relay are **configuration**: they are needed before there is a database to
 * read them out of, so they go into `.env`. contracts/settings.json draws the same line and
 * says why neither belongs in the settings table.
 */
#ifndef BACKEND_SETUP_H
#define BACKEND_SETUP_H

#include <stddef.h>

#include "backend_core/domain/community.h"
#include "service_core/db.h"
#include "service_core/email/config.h"
#include "service_core/env_file.h"

/** How many variables the conversation can decide. One per field it asks about. */
#define BACKEND_SETUP_ENTRY_MAX 16

/**
 * Everything the conversation produced: the row, and the variables as the text they go into
 * `.env` as.
 *
 * The values are buffers rather than pointers because `entries` points into them -- a
 * `sc_env_entry` borrows its strings, and the whole struct is one stack frame in
 * backend_setup(), which outlives every use of them.
 */
typedef struct backend_setup_answers {
    bc_home_community_setup community;

    char node_env[16];
    char db_type[16];
    char db_file[SC_DB_FILE_MAX];
    char db_host[SC_DB_HOST_MAX];
    char db_port[8];
    char db_user[SC_DB_USER_MAX];
    char db_password[SC_DB_PASSWORD_MAX];
    char db_database[SC_DB_NAME_MAX];

    char email[8];
    char email_host[SC_MAIL_HOST_MAX];
    char email_port[8];
    char email_tls[16];
    char email_user[SC_MAIL_USER_MAX];
    char email_pass[SC_MAIL_PASS_MAX];
    char email_sender[SC_MAIL_ADDR_MAX];
    char email_sender_name[SC_MAIL_SENDER_NAME_MAX];

    /** The variables to write, in the order they were decided. */
    sc_env_entry entries[BACKEND_SETUP_ENTRY_MAX];
    size_t entry_count;
} backend_setup_answers;

/**
 * Fills @p setup from a terminal, answering 1 when it did.
 *
 * 0 means there was nobody to ask: no terminal, or one that went away before the last answer.
 * It is not an error here -- the caller decides what a setup without an answer means.
 */
int backend_ask_for_setup(backend_setup_answers *setup);

#endif /* BACKEND_SETUP_H */
