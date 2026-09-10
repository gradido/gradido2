/*
 * How the community rows are loaded and persisted. The interaction decides *when*.
 *
 * Only the home community so far: every other row arrives through federation, which does not
 * exist yet. Both statements here are startup-only, which is why neither is on a hot path and
 * why neither caches anything -- the caller holds the result for the life of the process.
 *
 * Every statement is written twice, once per dialect, side by side in one sc_sql_statement --
 * service_core/sql.h. What differs between the two is the placeholder syntax and how a boolean
 * is spelled; the shape is the same, which is what makes the pair reviewable. Bytes and times go
 * in and come out as what they are, whichever database stores them how.
 */
#include "backend_core/domain/community.h"

#include <stdio.h>
#include <string.h>

#include "service_core/log/log.h"
#include "service_core/sql.h"

/* LIMIT 2 rather than 1: one row is the answer, two is a broken database, and asking for two is
 * what tells them apart. */
static sc_sql_statement kSelectHome = SC_SQL_STATEMENT(
    "community.find_home",
    "SELECT id, community_uuid, url, name, description, public_key FROM communities "
    "WHERE remote = false LIMIT 2",
    "SELECT id, community_uuid, url, name, description, public_key FROM communities "
    "WHERE remote = 0 LIMIT 2");

/* creation_date is when the community was founded, as far as this instance knows: now.
 * Distinct from created_at, which is when this row was written -- the two coincide only here. */
static sc_sql_statement kInsertHome = SC_SQL_STATEMENT(
    "community.insert_home",
    "INSERT INTO communities (remote, url, public_key, private_key, community_uuid, name, "
    "description, creation_date, created_at) VALUES (false, $1, $2, $3, $4, $5, $6, $7, $8) "
    "RETURNING id",
    "INSERT INTO communities (remote, url, public_key, private_key, community_uuid, name, "
    "description, creation_date, created_at) VALUES (0, ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
    "RETURNING id");

sc_status bc_community_find_home(sc_db *db, bc_home_community *out, int *found, char *error,
                                 size_t error_size)
{
    sc_sql_rows rows;
    sc_sql_error failure;
    sc_status status;

    if (db == NULL || out == NULL || found == NULL || error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;
    *found = 0;
    error[0] = '\0';
    memset(out, 0, sizeof(*out));

    status = sc_sql_query(db, &kSelectHome, NULL, 0, &rows, &failure);
    if (status != SC_OK) {
        bc_sql_set_error(error, error_size, failure.message);
        return status;
    }
    if (sc_sql_next(&rows)) {
        out->id = (uint64_t)sc_sql_col_int(&rows, 0);
        out->has_description = !sc_sql_col_is_null(&rows, 4);
        if (!sc_sql_col_copy(&rows, 1, out->community_uuid, sizeof(out->community_uuid)) ||
            !sc_sql_col_copy(&rows, 2, out->url, sizeof(out->url)) ||
            !sc_sql_col_copy(&rows, 3, out->name, sizeof(out->name)) ||
            (out->has_description &&
             !sc_sql_col_copy(&rows, 4, out->description, sizeof(out->description)))) {
            bc_sql_set_error(error, error_size,
                             "the home community row does not fit the contracted columns");
            status = SC_ERR_MALFORMED;
        } else if (sc_sql_col_bytes(&rows, 5, out->public_key, BC_PUBLIC_KEY_SIZE) !=
                   BC_PUBLIC_KEY_SIZE) {
            bc_sql_set_error(error, error_size,
                             "communities.public_key is not 32 bytes on the home community");
            status = SC_ERR_MALFORMED;
        } else {
            *found = 1;
        }
        if (status == SC_OK && sc_sql_next(&rows)) {
            bc_sql_set_error(error, error_size,
                             "more than one home community: communities.remote = false on "
                             "several rows");
            status = SC_ERR_MALFORMED;
            *found = 0;
        }
    }
    /* LIMIT 2 is read to its end, so a second row that failed to arrive is a failure here and
     * not "exactly one home community". */
    status = bc_sql_finish(&rows, &failure, status, error, error_size);
    if (status != SC_OK)
        *found = 0;
    return status;
}

/** Writes the home community. Called once, at first start, and never again. */
static sc_status insert_home(sc_db *db, const bc_home_community_setup *setup, const char *uuid,
                             const uint8_t *public_key, const uint8_t *private_key,
                             int64_t created_at, uint64_t *id_out, char *error, size_t error_size)
{
    sc_sql_param params[8];
    sc_sql_rows rows;
    sc_sql_error failure;
    sc_status status;

    params[0] = sc_sql_text(setup->url);
    params[1] = sc_sql_bytes(public_key, BC_PUBLIC_KEY_SIZE);
    params[2] = sc_sql_bytes(private_key, BC_PRIVATE_KEY_SIZE);
    params[3] = sc_sql_text(uuid);
    params[4] = sc_sql_text(setup->name);
    params[5] = sc_sql_text(setup->has_description ? setup->description : NULL);
    params[6] = sc_sql_time(created_at);
    params[7] = sc_sql_time(created_at);
    status = sc_sql_query(db, &kInsertHome, params, 8, &rows, &failure);
    if (status != SC_OK) {
        bc_sql_set_error(error, error_size, failure.message);
        return status;
    }
    *id_out = sc_sql_next(&rows) ? (uint64_t)sc_sql_col_int(&rows, 0) : 0;
    return bc_sql_finish(&rows, &failure, SC_OK, error, error_size);
}

sc_status bc_create_home_community(sc_db *db, const bc_home_community_setup *setup,
                                   bc_home_community *out, char *error, size_t error_size)
{
    uint8_t private_key[BC_PRIVATE_KEY_SIZE];
    int64_t created_at = sc_now_ms();
    sc_status status;

    if (db == NULL || setup == NULL || out == NULL || error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* Not a draw-and-check like users.gradido_id: communities_uuid_key is a plain unique index on
     * one column, so the database is the check. Legacy loops here because its equivalent index is
     * the same shape and it chose to look first anyway. */
    bc_new_uuid(out->community_uuid);
    bc_community_new_keys(out->public_key, private_key);

    status = insert_home(db, setup, out->community_uuid, out->public_key, private_key, created_at,
                         &out->id, error, error_size);
    /* The private key leaves this frame nowhere else: it is written and forgotten, and the
     * community the caller gets does not carry it. */
    memset(private_key, 0, sizeof(private_key));
    if (status != SC_OK)
        return status;

    (void)snprintf(out->url, sizeof(out->url), "%s", setup->url);
    (void)snprintf(out->name, sizeof(out->name), "%s", setup->name);
    out->has_description = setup->has_description;
    if (setup->has_description)
        (void)snprintf(out->description, sizeof(out->description), "%s", setup->description);

    {
        sc_log_value data[2] = {SC_LOG_STR("uuid", out->community_uuid),
                                SC_LOG_STR("url", out->url)};
        sc_log_context context = {0};

        context.data = data;
        context.data_count = 2;
        sc_log_event(SC_LOG_INFO, SC_CAT_COMMUNITY, "community.home.created", &context,
                     "home community \"%s\" created", out->name);
    }
    return SC_OK;
}
