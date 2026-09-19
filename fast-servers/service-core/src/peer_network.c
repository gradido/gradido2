#include "service_core/peer_network.h"

#include <uv.h>

static uv_once_t g_once = UV_ONCE_INIT;
static uv_mutex_t g_mutex;
static int g_ready;
static sc_peer_network_answer_fn g_fn;
static void *g_user_data;

static void init_mutex(void)
{
    g_ready = uv_mutex_init(&g_mutex) == 0;
}

sc_status sc_peer_network_register(sc_peer_network_answer_fn fn, void *user_data)
{
    uv_once(&g_once, init_mutex);
    if (!g_ready)
        return SC_ERR_NO_MEMORY;
    uv_mutex_lock(&g_mutex);
    g_fn = fn;
    g_user_data = fn != NULL ? user_data : NULL;
    uv_mutex_unlock(&g_mutex);
    return SC_OK;
}

sc_status sc_peer_network_bootstrap_answer(uint32_t max_peers, char *out, size_t cap, size_t *len)
{
    sc_status status = SC_ERR_UNAVAILABLE;

    if (out == NULL || len == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    uv_once(&g_once, init_mutex);
    if (!g_ready)
        return SC_ERR_UNAVAILABLE;
    /* Held for the whole answer: the role clears its registration before it shuts the node
     * down, and must not do so under a call that is still reading it. */
    uv_mutex_lock(&g_mutex);
    if (g_fn != NULL)
        status = g_fn(g_user_data, max_peers, out, cap, len);
    uv_mutex_unlock(&g_mutex);
    return status;
}
