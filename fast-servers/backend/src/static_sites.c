/*
 * Which site a path belongs to, and which file of that site -- everything the static web server
 * decides before it has anything to do with HTTP.
 *
 * A leaf on purpose: two pure functions over a table, reaching nothing. `static_routes.c` is
 * the half that answers a request, and it is separate so that this half can be tested without
 * linking a server -- `backend/tests/test_static_sites.cpp`, and AGENTS.md section 7 for the
 * rule that shape follows.
 */
#include "backend/static_sites.h"

#include <string.h>

const backend_static_site *backend_static_site_for(const backend_static_site *sites, size_t count,
                                                   const char *path, size_t path_length)
{
    const backend_static_site *match = NULL;
    size_t i;

    if (sites == NULL || path == NULL)
        return NULL;

    for (i = 0; i != count; ++i) {
        const backend_static_site *site = &sites[i];
        size_t base = site->base_path_length;

        /* A site at the root claims every path. One under a sub-path claims that path exactly,
         * and everything below it -- "/admin" and "/admin/…", but not "/administration". */
        if (base != 0) {
            if (path_length < base || memcmp(path, site->base_path, base) != 0)
                continue;
            if (path_length != base && path[base] != '/')
                continue;
        }
        if (match == NULL || base > match->base_path_length)
            match = site;
    }
    return match;
}

const backend_static_file *backend_static_file_find(const backend_static_site *site,
                                                    const char *path, size_t path_length)
{
    size_t low = 0;
    size_t high;

    if (site == NULL || path == NULL || site->file_count == 0)
        return NULL;

    high = site->file_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const backend_static_file *file = &site->files[middle];
        /* memcmp over the shorter length, then the lengths themselves: the table is sorted by
         * the same order, which is what a byte-wise comparison of two paths is. */
        size_t shortest = file->path_length < path_length ? file->path_length : path_length;
        int order = memcmp(file->path, path, shortest);

        if (order == 0) {
            if (file->path_length == path_length)
                return file;
            order = file->path_length < path_length ? -1 : 1;
        }
        if (order < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return NULL;
}
