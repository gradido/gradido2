#include "bootstrap.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "arnm/arena.h"
#include "arnm/json_reader.h"
#include "peer_id.h"
#include "service_core/http_client.h"
#include "service_core/log/log.h"

#define DHT_BOOTSTRAP_TIMEOUT_MS 10000L
/* What an answer may hold before it is refused as invalid: DHT_BOOTSTRAP_PEERS_MAX is 20, and
 * the room above it is for a community that answers with more. */
#define DHT_ANSWER_PEERS_MAX 64
#define DHT_ANSWER_ADDRESSES_MAX 32

/* ---------------------------------------------------------------- The answer */

sc_status dht_self_init(dht_self *self, lp2p *node, const uint8_t delegation[136])
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    memset(self, 0, sizeof(*self));
    if (uv_mutex_init(&self->mutex) != 0)
        return SC_ERR_NO_MEMORY;
    self->node = node;
    memcpy(self->node_key, delegation, 32);
    for (i = 0; i != 136; ++i) {
        self->delegation_hex[2 * i] = digits[delegation[i] >> 4];
        self->delegation_hex[2 * i + 1] = digits[delegation[i] & 0xf];
    }
    self->delegation_hex[2 * 136] = '\0';
    return SC_OK;
}

void dht_self_destroy(dht_self *self)
{
    uv_mutex_destroy(&self->mutex);
}

/* A multiaddr goes into the JSON as it is, so one that would need escaping is left out. */
static int plain(const char *text, size_t len)
{
    size_t i;

    for (i = 0; i != len; ++i) {
        if ((unsigned char)text[i] < 0x20 || text[i] == '"' || text[i] == '\\')
            return 0;
    }
    return len != 0;
}

void dht_self_add_address(dht_self *self, const char *address, size_t len)
{
    size_t i;

    if (len >= DHT_ADDRESS_MAX || !plain(address, len))
        return;
    uv_mutex_lock(&self->mutex);
    for (i = 0; i != self->address_count; ++i) {
        if (strlen(self->addresses[i]) == len && memcmp(self->addresses[i], address, len) == 0)
            break;
    }
    if (i == self->address_count && self->address_count != DHT_SELF_ADDRESSES_MAX) {
        memcpy(self->addresses[self->address_count], address, len);
        self->addresses[self->address_count][len] = '\0';
        ++self->address_count;
    }
    uv_mutex_unlock(&self->mutex);
}

typedef struct dht_writer {
    char *out;
    size_t cap;
    size_t len;
    int overflow;
} dht_writer;

static void put(dht_writer *w, const char *format, ...)
{
    va_list args;
    int written;

    if (w->overflow)
        return;
    va_start(args, format);
    written = vsnprintf(w->out + w->len, w->cap - w->len, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= w->cap - w->len)
        w->overflow = 1;
    else
        w->len += (size_t)written;
}

sc_status dht_bootstrap_answer(void *user_data, uint32_t max_peers, char *out, size_t cap,
                               size_t *len)
{
    dht_self *self = (dht_self *)user_data;
    dht_writer w = {out, cap, 0, 0};
    char peer_id[DHT_PEER_ID_TEXT_MAX];
    /* Records as lp2p_poll writes them: a header, then the multiaddrs, NUL-separated. */
    uint8_t sample[32 * 1024];
    const unsigned long long now_ms = (unsigned long long)time(NULL) * 1000ull;
    int32_t written;
    size_t offset = 0;
    size_t i;
    int first = 1;

    dht_peer_id_format(self->node_key, peer_id);
    put(&w, "{\"self\":{\"peerId\":\"%s\",\"addresses\":[", peer_id);
    uv_mutex_lock(&self->mutex);
    for (i = 0; i != self->address_count; ++i)
        put(&w, "%s\"%s\"", i == 0 ? "" : ",", self->addresses[i]);
    uv_mutex_unlock(&self->mutex);
    put(&w, "],\"delegation\":\"%s\"},\"peers\":[", self->delegation_hex);

    /* The module rotates where the sample starts, so two calls hand out different peers. */
    written = lp2p_routing_sample(self->node, sample, sizeof(sample), max_peers);
    while (written > 0 && offset + sizeof(lp2p_event) <= (size_t)written) {
        lp2p_event event;
        const char *data;
        size_t at = 0;
        int any = 0;

        memcpy(&event, sample + offset, sizeof(event));
        if (event.size < sizeof(event) || event.size > (size_t)written - offset ||
            event.data_len > event.size - sizeof(event))
            break;
        data = (const char *)sample + offset + sizeof(event);
        offset += event.size;
        if (event.type != LP2P_EV_PEER_DISCOVERED)
            continue;

        dht_peer_id_format(event.node, peer_id);
        put(&w, "%s{\"peerId\":\"%s\",\"addresses\":[", first ? "" : ",", peer_id);
        while (at < event.data_len) {
            const char *end = memchr(data + at, '\0', event.data_len - at);
            const size_t length = end != NULL ? (size_t)(end - (data + at)) : event.data_len - at;

            if (plain(data + at, length)) {
                put(&w, "%s\"%.*s\"", any ? "," : "", (int)length, data + at);
                any = 1;
            }
            at += length + 1;
        }
        put(&w, "],\"lastSeenAt\":\"%llu\"}", now_ms);
        first = 0;
    }
    put(&w, "]}");
    if (w.overflow)
        return SC_ERR_TOO_LONG;
    *len = w.len;
    return SC_OK;
}

/* ---------------------------------------------------------------- Joining */

/* An arena reader released over an arena it never frees answers a warning, not a failure. */
static int arnm_ok(arnm_result result)
{
    return result == ARNM_SUCCESS || result == ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED;
}

static void bootstrap_failed(const char *url, const char *reason)
{
    sc_log_value values[] = {SC_LOG_STR("url", url), SC_LOG_STR("reason", reason)};
    sc_log_context context = {0};

    context.data = values;
    context.data_count = 2;
    sc_log_event(SC_LOG_WARN, SC_CAT_DHT, "dht.bootstrap.failed", &context,
                 "bootstrap from %s failed: %s", url, reason);
}

/*
 * One entry of the answer into the routing table. Its addresses may end in /p2p/<its id>, which
 * the module does not want beside the key. Answers whether any address was taken.
 */
static int take_entry(lp2p *node, const uint8_t key[32], const arnm_memory_block *addresses,
                      uint32_t count)
{
    char address[DHT_ADDRESS_MAX];
    char suffix[DHT_PEER_ID_TEXT_MAX + 5];
    char peer_id[DHT_PEER_ID_TEXT_MAX];
    size_t suffix_len;
    uint32_t i;
    int taken = 0;

    dht_peer_id_format(key, peer_id);
    suffix_len = (size_t)snprintf(suffix, sizeof(suffix), "/p2p/%s", peer_id);
    for (i = 0; i != count; ++i) {
        size_t length = addresses[i].size;

        if (length >= sizeof(address))
            continue;
        memcpy(address, addresses[i].data, length);
        address[length] = '\0';
        if (length > suffix_len && memcmp(address + length - suffix_len, suffix, suffix_len) == 0)
            address[length - suffix_len] = '\0';
        if (lp2p_add_address(node, key, address) == LP2P_OK)
            taken = 1;
    }
    return taken;
}

/* The addresses of one entry, and its peer id as a key. 0 when the entry is malformed. */
static int read_entry(arnm_json_value *entry, uint8_t key[32], arnm_memory_block *addresses,
                      uint32_t *count)
{
    arnm_memory_block peer_id = {0};
    arnm_json_value *list = NULL;
    arnm_json_field fields[] = {ARNM_JSON_FIELD_STRING("peerId", &peer_id),
                                {"addresses", 9, ARNM_JSON_FIELD_TYPE_VALUE, &list}};
    uint64_t found = 0;

    if (!arnm_ok(arnm_json_read_object(entry, fields, 2, &found)) || found != 3)
        return 0;
    if (!dht_peer_id_parse((const char *)peer_id.data, peer_id.size, key))
        return 0;
    return arnm_ok(arnm_json_read_array(list, ARNM_JSON_FIELD_TYPE_STRING, addresses,
                                        DHT_ANSWER_ADDRESSES_MAX, count));
}

void dht_bootstrap_join(lp2p *node, const uint8_t own_key[32], const char *url,
                        const sc_quit_flag *quit, dht_bootstrap_buffers *buffers)
{
    char route[512];
    size_t body_len = 0;
    long http_status = 0;
    arnm allocator;
    arnm_json_reader reader;
    arnm_json_value *root = NULL;
    arnm_json_value *self_value = NULL;
    arnm_json_value *peers_value = NULL;
    arnm_json_value *peers[DHT_ANSWER_PEERS_MAX];
    arnm_memory_block addresses[DHT_ANSWER_ADDRESSES_MAX];
    uint8_t delegation[136];
    arnm_memory_block delegation_block = {delegation, sizeof(delegation)};
    uint8_t key[32];
    uint8_t named[32];
    uint32_t peer_count = 0;
    uint32_t address_count = 0;
    uint32_t taken = 0;
    uint32_t i;
    uint64_t found = 0;
    size_t url_len = strlen(url);

    /* The route is peer.bootstrap under the community's URL, with or without its trailing slash. */
    while (url_len != 0 && url[url_len - 1] == '/')
        --url_len;
    if ((size_t)snprintf(route, sizeof(route), "%.*s/peer/bootstrap", (int)url_len, url) >=
        sizeof(route)) {
        bootstrap_failed(url, "unreachable");
        return;
    }
    if (sc_http_get(route, DHT_BOOTSTRAP_TIMEOUT_MS, quit, buffers->body, buffers->body_bytes,
                    &body_len, &http_status) != SC_OK ||
        http_status != 200) {
        if (!sc_quit_requested(quit))
            bootstrap_failed(url, http_status == 200 || http_status == 0 ? "unreachable"
                                                                         : "invalid-answer");
        return;
    }

    if (!arnm_ok(arnm_init_arena_borrow(&allocator, buffers->arena, (uint32_t)buffers->arena_bytes)) ||
        !arnm_ok(arnm_json_reader_init(&reader, &allocator))) {
        bootstrap_failed(url, "invalid-answer");
        return;
    }
    if (!arnm_ok(arnm_json_reader_parse(&reader, buffers->body, (uint32_t)body_len, false, &root))) {
        arnm_json_reader_release(&reader);
        bootstrap_failed(url, "invalid-answer");
        return;
    }
    {
        arnm_json_field fields[] = {{"self", 4, ARNM_JSON_FIELD_TYPE_VALUE, &self_value},
                                    {"peers", 5, ARNM_JSON_FIELD_TYPE_VALUE, &peers_value}};
        if (!arnm_ok(arnm_json_read_object(root, fields, 2, &found)) || found != 3 ||
            !arnm_ok(arnm_json_read_array(peers_value, ARNM_JSON_FIELD_TYPE_VALUE, peers,
                                          DHT_ANSWER_PEERS_MAX, &peer_count)) ||
            !read_entry(self_value, key, addresses, &address_count)) {
            arnm_json_reader_release(&reader);
            bootstrap_failed(url, "invalid-answer");
            return;
        }
    }
    {
        arnm_json_field fields[] = {
            {"delegation", 10, ARNM_JSON_FIELD_TYPE_HEX_FIXED, &delegation_block}};
        found = 0;
        if (!arnm_ok(arnm_json_read_object(self_value, fields, 1, &found)) || found != 1) {
            arnm_json_reader_release(&reader);
            bootstrap_failed(url, "delegation-invalid");
            return;
        }
    }
    /* `self` is the one entry that is checked: its community signed for it. The peers are hints,
     * and each is verified by the handshake when it is dialled. */
    if (lp2p_delegation_verify(delegation, sizeof(delegation), 0, NULL, named) != LP2P_OK ||
        memcmp(named, key, sizeof(key)) != 0) {
        arnm_json_reader_release(&reader);
        bootstrap_failed(url, "delegation-invalid");
        return;
    }

    if (memcmp(key, own_key, sizeof(key)) != 0)
        taken += (uint32_t)take_entry(node, key, addresses, address_count);
    for (i = 0; i != peer_count; ++i) {
        if (!read_entry(peers[i], key, addresses, &address_count))
            continue;
        if (memcmp(key, own_key, sizeof(key)) != 0)
            taken += (uint32_t)take_entry(node, key, addresses, address_count);
    }
    arnm_json_reader_release(&reader);
    if (taken != 0) {
        uint64_t query_id;
        (void)lp2p_dht_bootstrap(node, &query_id);
    }

    {
        sc_log_value values[] = {SC_LOG_STR("url", url), SC_LOG_UINT("peers", taken)};
        sc_log_context context = {0};

        context.data = values;
        context.data_count = 2;
        sc_log_event(SC_LOG_INFO, SC_CAT_DHT, "dht.bootstrap.applied", &context,
                     "bootstrapped from %s: %u nodes taken up", url, (unsigned)taken);
    }
}
