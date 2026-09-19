#include "service_core/http_client.h"

#include <string.h>

#include <curl/curl.h>

typedef struct sc_http_sink {
    char *out;
    size_t cap;
    size_t len;
    int overflow;
    const sc_quit_flag *quit;
} sc_http_sink;

static size_t collect(char *data, size_t size, size_t count, void *user)
{
    sc_http_sink *sink = (sc_http_sink *)user;
    const size_t bytes = size * count;

    if (bytes > sink->cap - sink->len) {
        sink->overflow = 1;
        return 0; /* curl aborts the transfer */
    }
    memcpy(sink->out + sink->len, data, bytes);
    sink->len += bytes;
    return bytes;
}

static int progress(void *user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                    curl_off_t ulnow)
{
    const sc_http_sink *sink = (const sc_http_sink *)user;

    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return sink->quit != NULL && sc_quit_requested(sink->quit);
}

sc_status sc_http_get(const char *url, long timeout_ms, const sc_quit_flag *quit, char *out,
                      size_t cap, size_t *len, long *http_status)
{
    sc_http_sink sink = {out, cap, 0, 0, quit};
    CURL *curl;
    CURLcode code;

    if (url == NULL || out == NULL || len == NULL || http_status == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    curl = curl_easy_init();
    if (curl == NULL)
        return SC_ERR_NO_MEMORY;
    (void)curl_easy_setopt(curl, CURLOPT_URL, url);
    (void)curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    (void)curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    (void)curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &sink);
    (void)curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    code = curl_easy_perform(curl);
    *http_status = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
    curl_easy_cleanup(curl);
    *len = sink.len;
    if (sink.overflow)
        return SC_ERR_TOO_LONG;
    return code == CURLE_OK ? SC_OK : SC_ERR_NETWORK;
}
