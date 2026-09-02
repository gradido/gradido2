/*
 * The fallback HTTP backend: libuv for the platform, picohttpparser for the request head.
 *
 * It exists for the build h2o cannot serve. h2o is a posix event loop -- epoll or kqueue,
 * sys/socket.h -- and does not compile against the Windows CRT, so the MSVC build would
 * otherwise have no server at all. libuv does compile there, picohttpparser compiles anywhere,
 * and between them the C servers are *runnable* on Windows. That is a different claim from
 * fast: one thread, one loop, no TLS, no HTTP/2, and h2o answers a cached request in 11.6 us
 * where this does not try to.
 *
 * What picohttpparser does is parse a head and say how long it was. Everything below that call
 * is this file: owning the buffer, deciding the body length, waiting for the body, decoding
 * chunked, answering, and then finding the next request in what is left.
 */
#include "service_core/http.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "http_defer.h"
#include "picohttpparser.h"
#include "service_core/log.h"

#define REQ_BUF (64 * 1024)
#define MAX_HEAD 8192
/* One number for both backends -- see service_core/http.h. */
#define MAX_BODY SC_HTTP_MAX_BODY
#define MAX_HEADERS 32
#define OUT_BUF (16 * 1024)
#define REPLY_MAX (8 * 1024)
#define DRAIN_MS 2000          /* how long to keep listening after the last answer */
#define DRAIN_MAX (256 * 1024) /* and how much to swallow before giving up on politeness */
#define MAX_CHUNK_LINE 64      /* a size line, extensions included, is short or it is hostile */
#define MAX_TRAILER 2048
#define MAX_ROUTES 64
/* Room for SC_HTTP_RESPONSE_HEADERS_MAX headers, already serialised. Generous for what a
 * handler adds -- the CORS block is six of them and the longest carries a list of origins. */
#define EXTRA_HEADERS_MAX 2048

enum framing { FRAME_NONE, FRAME_LENGTH, FRAME_CHUNKED };
enum ch_state { CH_SIZE, CH_DATA, CH_DATA_CRLF, CH_TRAILER, CH_DONE };

typedef struct sc_route {
    const char *path;
    size_t path_len;
    sc_http_handler_fn fn;
    void *user_data;
} sc_route;

struct sc_http_server {
    uv_loop_t loop;
    uv_tcp_t listener;
    uv_timer_t quit_timer;
    /* What a worker thread touches, and the only thing here that it may. uv_async_send is
     * documented as safe from any thread; nothing else in libuv's loop-bound half is. */
    uv_async_t resume_async;
    const sc_quit_flag *quit;

    sc_route routes[MAX_ROUTES];
    size_t route_count;
    /* What answers a path no route matched. NULL leaves the plain 404 -- see
     * sc_http_route_default. */
    sc_http_handler_fn default_fn;
    void *default_data;
    /* What runs before any route -- see sc_http_before_route. */
    sc_http_handler_fn before_route;
    void *before_route_data;

    /* One loop, so one table, and its loop index is always zero. */
    sc_defer_table defer;
    sc_http_resume_fn on_resume;
    void *on_resume_data;

    char host[64];
    char role[32];
    uint16_t port;
    int loop_started;
    int listening;
};

/*
 * What a handler sees. It carries the parsed head, the collected body and the response the
 * handler is filling in, and it lives inside the connection -- so it is valid for exactly as
 * long as the request is, which is the handler's own call unless the handler deferred it. That
 * is the same promise h2o's request pool makes.
 */
struct sc_http_req {
    const char *method;
    size_t method_len;
    const char *path;
    size_t path_len;
    int minor_version;
    struct phr_header headers[MAX_HEADERS];
    size_t num_headers;
    const char *body;
    size_t body_len;

    /* filled by sc_http_header_add, and written out by respond() ahead of the blank line */
    size_t extra_header_count;
    size_t extra_headers_len;
    char extra_headers[EXTRA_HEADERS_MAX];

    /* filled by sc_http_reply */
    int replied;
    int status;
    const char *content_type;
    size_t reply_len;
    char reply[REPLY_MAX];
};

struct conn {
    uv_tcp_t handle;
    uv_write_t wr;
    uv_shutdown_t sr;
    uv_timer_t drain_timer;
    int open_handles; /* the tcp and the timer; the memory goes when both are shut */
    sc_http_server *server;

    /*
     * The request lives here rather than on the read callback's stack, and that is what makes
     * deferring possible at all: a handler that answers later needs its request to outlive the
     * call it was made in. Everything in it still points into `in`, which is why reading is
     * parsed but not consumed while a request is deferred -- see process().
     */
    struct sc_http_req req;
    int deferred;             /* a handler took the request and has not answered yet */
    sc_http_ticket ticket;    /* what a worker is holding while it is deferred */
    size_t deferred_wire_len; /* what to consume once the answer goes out */

    char in[REQ_BUF];
    size_t in_used;
    size_t last_len; /* what the previous incomplete parse had already seen */

    size_t head_len;    /* 0 until the head is complete */
    int framing;        /* enum framing, decided once when the head lands */
    size_t content_len; /* FRAME_LENGTH: what Content-Length said */

    /* FRAME_CHUNKED. The body is decoded in place, over the wire form: the write cursor never
     * catches the read cursor, because every chunk costs at least a size line more than its
     * payload. */
    int ch_state;
    size_t ch_read;      /* next wire byte to look at, absolute offset into in */
    size_t ch_remaining; /* payload left in the chunk being copied */
    size_t body_len;     /* decoded so far; the body starts at in + head_len */

    char out[OUT_BUF];
    size_t out_len;

    int writing;
    int close_after_write;
    int draining;
    int closing;
    size_t drained;
};

static void process(struct conn *c);
static void on_read(uv_stream_t *s, ssize_t n, const uv_buf_t *b);

const char *sc_http_backend_name(void)
{
    return "libuv+picohttpparser";
}

/* strncasecmp is POSIX and _strnicmp is Windows; this file is the one place that cannot afford
 * either. */
static int ci_eq(const char *a, size_t a_len, const char *b, size_t b_len)
{
    size_t i;

    if (a_len != b_len)
        return 0;
    for (i = 0; i != a_len; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (char)(y + 32);
        if (x != y)
            return 0;
    }
    return 1;
}

static const char *header_of(const struct sc_http_req *req, const char *name, size_t *len)
{
    size_t i;

    for (i = 0; i != req->num_headers; ++i) {
        if (ci_eq(req->headers[i].name, req->headers[i].name_len, name, strlen(name))) {
            if (len != NULL)
                *len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }
    return NULL;
}

static void on_handle_closed(uv_handle_t *h)
{
    struct conn *c = (struct conn *)h->data;
    if (--c->open_handles == 0)
        free(c);
}

/** Shut both handles down and free the connection when the last one is gone. */
static void teardown(struct conn *c)
{
    if (c->closing)
        return;
    c->closing = 1;
    if (c->deferred) {
        /* A worker is still holding this request's ticket. Its slot stays taken so the resume
         * has somewhere to land; what goes is the request, and the resume callback will be
         * told the client is gone rather than handed a connection that is being freed. */
        c->deferred = 0;
        sc_defer_forget(&c->server->defer, sc_defer_index_of(c->ticket),
                        sc_defer_generation_of(c->ticket));
    }
    uv_timer_stop(&c->drain_timer);
    uv_close((uv_handle_t *)&c->drain_timer, on_handle_closed);
    uv_close((uv_handle_t *)&c->handle, on_handle_closed);
}

static void on_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *b)
{
    struct conn *c = (struct conn *)h;
    (void)suggested;
    b->base = c->in + c->in_used;
    b->len = REQ_BUF - c->in_used;
}

static void on_alloc_discard(uv_handle_t *h, size_t suggested, uv_buf_t *b)
{
    struct conn *c = (struct conn *)h;
    (void)suggested;
    b->base = c->out; /* the answer is on the wire; this buffer is free again */
    b->len = OUT_BUF;
}

static void on_read_discard(uv_stream_t *s, ssize_t n, const uv_buf_t *b)
{
    struct conn *c = (struct conn *)s;
    (void)b;
    if (n < 0) { /* EOF: the client saw the FIN and let go */
        teardown(c);
        return;
    }
    c->drained += (size_t)n;
    if (c->drained > DRAIN_MAX)
        teardown(c); /* politeness has a budget */
}

static void on_drain_timeout(uv_timer_t *t)
{
    teardown((struct conn *)((char *)t - offsetof(struct conn, drain_timer)));
}

static void on_shutdown(uv_shutdown_t *req, int status)
{
    struct conn *c = (struct conn *)req->data;
    if (status < 0) {
        teardown(c);
        return;
    }
    /* The FIN is out. Keep reading and throwing away what is still arriving, so that close()
     * finds an empty receive buffer and sends nothing but a FIN. Closing on top of unread bytes
     * sends RST instead, and an RST discards whatever of the answer the client had not read yet
     * -- which is how an error response to a request still being sent gets lost. */
    uv_timer_start(&c->drain_timer, on_drain_timeout, DRAIN_MS, 0);
    uv_read_start((uv_stream_t *)&c->handle, on_alloc_discard, on_read_discard);
}

/** Answer sent, connection over: FIN first, close once the client agrees. */
static void begin_drain(struct conn *c)
{
    if (c->draining || c->closing)
        return;
    c->draining = 1;
    uv_read_stop((uv_stream_t *)&c->handle);
    c->sr.data = c;
    if (uv_shutdown(&c->sr, (uv_stream_t *)&c->handle, on_shutdown) != 0)
        teardown(c);
}

static void on_written(uv_write_t *w, int status)
{
    struct conn *c = (struct conn *)w->data;
    c->writing = 0;
    if (status < 0) {
        teardown(c);
        return;
    }
    if (c->close_after_write) {
        begin_drain(c);
        return;
    }
    /* Reading was stopped while the answer went out, so the buffer could not grow behind our
     * back. Whatever is already in it may be a pipelined request; process that before asking
     * for more. */
    process(c);
    if (!c->writing && !c->close_after_write)
        uv_read_start((uv_stream_t *)&c->handle, on_alloc, on_read);
}

/** Serialise a status line, the headers and @p body into c->out and put it on the wire. */
static void respond(struct conn *c, int status, const char *content_type, const char *body,
                    size_t body_len, int keep_alive, const char *extra_headers)
{
    uv_buf_t buf;
    const char *extra = extra_headers != NULL ? extra_headers : "";
    /* A 204 carries no body, and RFC 7230 forbids describing one: no Content-Length, and no
     * Content-Type for a representation that is not there. h2o omits both for the same status,
     * which is what keeps the two backends indistinguishable to a client. */
    int n = status == 204
                ? snprintf(c->out, OUT_BUF,
                           "HTTP/1.1 %d %s\r\n"
                           "Connection: %s\r\n"
                           "%s"
                           "\r\n",
                           status, sc_http_reason(status), keep_alive ? "keep-alive" : "close", extra)
                : snprintf(c->out, OUT_BUF,
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: %s\r\n"
                           "%s"
                           "\r\n",
                           status, sc_http_reason(status), content_type, body_len,
                           keep_alive ? "keep-alive" : "close", extra);
    /* A body a 204 must not carry is dropped here rather than refused: the status is the
     * caller's decision and the framing is this function's, so the two cannot disagree on the
     * wire whatever a handler passed in. */
    if (status == 204)
        body_len = 0;
    if (n < 0 || (size_t)n + body_len > OUT_BUF) {
        /* Cannot even say what went wrong within the buffer. */
        teardown(c);
        return;
    }
    if (body_len != 0)
        memcpy(c->out + n, body, body_len);
    c->out_len = (size_t)n + body_len;

    if (!keep_alive)
        c->close_after_write = 1;

    uv_read_stop((uv_stream_t *)&c->handle);
    buf = uv_buf_init(c->out, (unsigned)c->out_len);
    c->writing = 1;
    c->wr.data = c;
    uv_write(&c->wr, (uv_stream_t *)&c->handle, &buf, 1, on_written);
}

static void fail(struct conn *c, int status, const char *body)
{
    /* An error ends the connection: the framing is no longer trustworthy. */
    /* An error before or beside a request carries none of what a handler would have added. */
    respond(c, status, "text/plain", body, strlen(body), 0, NULL);
}

/** @return -1 when the value is not a number we are willing to accept */
static long long content_length_of(const struct sc_http_req *req)
{
    size_t len = 0;
    const char *v = header_of(req, "content-length", &len);
    long long n = 0;
    size_t i;

    if (v == NULL)
        return 0;
    if (len == 0 || len > 19)
        return -1;
    for (i = 0; i != len; ++i) {
        if (v[i] < '0' || v[i] > '9')
            return -1;
        n = n * 10 + (v[i] - '0');
        if (n > MAX_BODY)
            return -1;
    }
    return n;
}

static int wants_keep_alive(const struct sc_http_req *req)
{
    size_t len = 0;
    const char *v = header_of(req, "connection", &len);

    if (v != NULL) {
        if (ci_eq(v, len, "close", 5))
            return 0;
        if (ci_eq(v, len, "keep-alive", 10))
            return 1;
    }
    return req->minor_version >= 1; /* HTTP/1.1 keeps it open by default, 1.0 does not */
}

/** Drop @p n bytes from the front of the input buffer and reset the parse. */
static void consume(struct conn *c, size_t n)
{
    memmove(c->in, c->in + n, c->in_used - n);
    c->in_used -= n;
    c->last_len = 0;
    c->head_len = 0;
    c->framing = FRAME_NONE;
    c->content_len = 0;
    c->ch_state = CH_SIZE;
    c->ch_read = 0;
    c->ch_remaining = 0;
    c->body_len = 0;
}

/** @return non-zero if the value is exactly `chunked`, ignoring case and spaces.
 *
 *  A list -- `gzip, chunked` -- is legal HTTP and is not handled here. Saying so with 501 is
 *  honest; guessing would be the beginning of a smuggling bug. */
static int te_is_plain_chunked(const char *v, size_t len)
{
    while (len != 0 && (*v == ' ' || *v == '\t')) {
        ++v;
        --len;
    }
    while (len != 0 && (v[len - 1] == ' ' || v[len - 1] == '\t'))
        --len;
    return ci_eq(v, len, "chunked", 7);
}

enum decode { DEC_NEED_MORE, DEC_DONE, DEC_BAD, DEC_TOO_LARGE };

/** Decode as much of the chunked body as has arrived, in place.
 *
 *  In place is safe because the decoded output always trails the wire cursor: every chunk costs
 *  a size line and a CRLF more than its payload, so the write position can only fall further
 *  behind the read position, never catch it. */
static enum decode decode_chunked(struct conn *c)
{
    for (;;) {
        switch (c->ch_state) {
        case CH_SIZE: {
            const char *line = c->in + c->ch_read;
            size_t avail = c->in_used - c->ch_read;
            const char *nl = memchr(line, '\n', avail);
            size_t line_len;
            size_t size = 0, digits = 0;
            size_t i;

            if (nl == NULL)
                return avail > MAX_CHUNK_LINE ? DEC_BAD : DEC_NEED_MORE;
            line_len = (size_t)(nl - line) + 1;
            if (line_len > MAX_CHUNK_LINE)
                return DEC_BAD;

            for (i = 0; i != line_len; ++i) {
                char ch = line[i];
                int d;
                if (ch >= '0' && ch <= '9')
                    d = ch - '0';
                else if (ch >= 'a' && ch <= 'f')
                    d = ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F')
                    d = ch - 'A' + 10;
                else
                    break; /* ';' opens an extension, CR ends the line */
                size = size * 16 + (size_t)d;
                ++digits;
                if (size > MAX_BODY)
                    return DEC_TOO_LARGE;
            }
            if (digits == 0)
                return DEC_BAD;

            c->ch_read += line_len;
            if (size == 0) {
                c->ch_state = CH_TRAILER;
                break;
            }
            if (c->body_len + size > MAX_BODY)
                return DEC_TOO_LARGE;
            c->ch_remaining = size;
            c->ch_state = CH_DATA;
            break;
        }

        case CH_DATA: {
            size_t avail = c->in_used - c->ch_read;
            size_t n = avail < c->ch_remaining ? avail : c->ch_remaining;
            if (n != 0) {
                memmove(c->in + c->head_len + c->body_len, c->in + c->ch_read, n);
                c->body_len += n;
                c->ch_read += n;
                c->ch_remaining -= n;
            }
            if (c->ch_remaining != 0)
                return DEC_NEED_MORE;
            c->ch_state = CH_DATA_CRLF;
            break;
        }

        case CH_DATA_CRLF: {
            if (c->in_used - c->ch_read < 2)
                return DEC_NEED_MORE;
            if (c->in[c->ch_read] != '\r' || c->in[c->ch_read + 1] != '\n')
                return DEC_BAD;
            c->ch_read += 2;
            c->ch_state = CH_SIZE;
            break;
        }

        case CH_TRAILER: {
            /* Trailer fields, then an empty line. Nothing reads them: honouring a trailer that
             * changes how the request is understood is the other half of request smuggling. */
            for (;;) {
                size_t avail = c->in_used - c->ch_read;
                const char *nl = memchr(c->in + c->ch_read, '\n', avail);
                size_t line_len;
                if (nl == NULL)
                    return avail > MAX_TRAILER ? DEC_BAD : DEC_NEED_MORE;
                line_len = (size_t)(nl - (c->in + c->ch_read)) + 1;
                c->ch_read += line_len;
                if (line_len <= 2) { /* "\n" or "\r\n": the end of the request */
                    c->ch_state = CH_DONE;
                    return DEC_DONE;
                }
            }
        }

        case CH_DONE:
        default:
            return DEC_DONE;
        }
    }
}

/**
 * Offers the request to the registered routes in registration order, first exact path match
 * wins, and a handler answering -1 passes it to the next one -- the same convention h2o uses,
 * so a handler behaves identically behind either backend.
 *
 * The query string is not part of the match. Neither backend gives a route a pattern: a path is
 * a path, and everything variable about a request is in its body.
 */
static void dispatch(struct conn *c, struct sc_http_req *req)
{
    const char *query = memchr(req->path, '?', req->path_len);
    size_t path_len = query != NULL ? (size_t)(query - req->path) : req->path_len;
    size_t i;

    /* What applies to every request, before the one that applies to this path. A hook that
     * answered has answered; nothing else runs. */
    if (c->server->before_route != NULL &&
        c->server->before_route(req, c->server->before_route_data) == 0)
        return;

    for (i = 0; i != c->server->route_count; ++i) {
        const sc_route *route = &c->server->routes[i];
        if (route->path_len != path_len || memcmp(route->path, req->path, path_len) != 0)
            continue;
        if (route->fn(req, route->user_data) == 0)
            return;
    }
    if (c->server->default_fn != NULL &&
        c->server->default_fn(req, c->server->default_data) == 0)
        return;
    req->replied = 1;
    req->status = 404;
    req->content_type = "text/plain";
    req->reply_len = 0;
}

static void process(struct conn *c)
{
    /* A deferred request has not been answered, so its bytes are still in the buffer and its
     * pointers still point into them. Nothing may be parsed or consumed until it is done --
     * and nothing may be answered either, because HTTP/1.1 responses go out in request order.
     * Reading stays on, which is how an EOF is still noticed while a worker is busy. */
    while (!c->writing && !c->close_after_write && !c->deferred) {
        struct sc_http_req *req = &c->req;
        size_t num = MAX_HEADERS;
        size_t wire_len; /* what this request occupies, head included */
        int keep;

        memset(req, 0, sizeof(*req));

        if (c->head_len == 0) {
            size_t te_len = 0, cl_len = 0;
            const char *te, *cl_hdr;
            int r = phr_parse_request(c->in, c->in_used, &req->method, &req->method_len, &req->path,
                                      &req->path_len, &req->minor_version, req->headers, &num,
                                      c->last_len);
            if (r == -2) { /* the head is not complete yet */
                c->last_len = c->in_used;
                if (c->in_used >= MAX_HEAD)
                    fail(c, 431, "head too large\n");
                return;
            }
            if (r == -1) {
                fail(c, 400, "malformed request\n");
                return;
            }
            c->head_len = (size_t)r;
            req->num_headers = num;

            te = header_of(req, "transfer-encoding", &te_len);
            cl_hdr = header_of(req, "content-length", &cl_len);

            if (te != NULL) {
                /* Both headers on one request is how a proxy and a server are made to disagree
                 * about where it ends. Refuse rather than pick a winner: this server is nobody's
                 * front end. */
                if (cl_hdr != NULL) {
                    fail(c, 400, "Content-Length with Transfer-Encoding\n");
                    return;
                }
                if (!te_is_plain_chunked(te, te_len)) {
                    fail(c, 501, "only chunked transfer encoding\n");
                    return;
                }
                if (req->minor_version < 1) {
                    fail(c, 400, "chunked needs HTTP/1.1\n");
                    return;
                }
                c->framing = FRAME_CHUNKED;
                c->ch_state = CH_SIZE;
                c->ch_read = c->head_len;
                c->ch_remaining = 0;
                c->body_len = 0;
            } else {
                long long n = content_length_of(req);
                if (n < 0) {
                    fail(c, 413, "bad or oversized Content-Length\n");
                    return;
                }
                c->framing = FRAME_LENGTH;
                c->content_len = (size_t)n;
            }
        } else {
            /* The head was parsed on an earlier read; parse it again off the same bytes, which
             * is cheap and keeps the pointers valid. The head is never written over -- the
             * decoded body starts after it. */
            phr_parse_request(c->in, c->in_used, &req->method, &req->method_len, &req->path,
                              &req->path_len, &req->minor_version, req->headers, &num, 0);
            req->num_headers = num;
        }

        if (c->framing == FRAME_CHUNKED) {
            switch (decode_chunked(c)) {
            case DEC_NEED_MORE:
                return;
            case DEC_BAD:
                fail(c, 400, "malformed chunked body\n");
                return;
            case DEC_TOO_LARGE:
                fail(c, 413, "chunked body too large\n");
                return;
            case DEC_DONE:
                break;
            }
            wire_len = c->ch_read;
            req->body_len = c->body_len;
        } else {
            if (c->in_used - c->head_len < c->content_len)
                return; /* body still arriving */
            wire_len = c->head_len + c->content_len;
            req->body_len = c->content_len;
        }
        req->body = req->body_len != 0 ? c->in + c->head_len : NULL;

        dispatch(c, req);
        if (c->deferred) {
            /* The handler took it. Remember what to drop once the answer goes out -- by then
             * the head will not be re-parsed and nothing else knows how long this request was. */
            c->deferred_wire_len = wire_len;
            return;
        }
        if (!req->replied) {
            /* A route that accepted a request must have answered it. One that returns 0 without
             * replying would otherwise leave the client waiting, which is harder to find than a
             * 500 in the log. */
            req->status = 500;
            req->content_type = "text/plain";
            req->reply_len = 0;
        }

        keep = wants_keep_alive(req);
        /* Copies the answer out before the buffer moves. */
        respond(c, req->status, req->content_type, req->reply, req->reply_len, keep,
                req->extra_headers);
        consume(c, wire_len);
    }
}

static void on_read(uv_stream_t *s, ssize_t n, const uv_buf_t *b)
{
    struct conn *c = (struct conn *)s;
    (void)b;
    if (n < 0) {
        if (!c->writing)
            teardown(c);
        else
            c->close_after_write = 1;
        return;
    }
    if (n == 0)
        return;
    c->in_used += (size_t)n;
    process(c);
}

static void on_connection(uv_stream_t *listener, int status)
{
    sc_http_server *server = (sc_http_server *)listener->data;
    struct conn *c;

    if (status < 0)
        return;
    /* One allocation per connection, and it is 80 KiB of buffers. That is the shape this backend
     * has rather than the arena discipline AGENTS.md section 1 asks for on the request path --
     * and it is affordable only because this backend is the one not carrying load. */
    c = calloc(1, sizeof(struct conn));
    if (c == NULL)
        return;
    c->server = server;
    uv_tcp_init(&server->loop, &c->handle);
    uv_timer_init(&server->loop, &c->drain_timer);
    c->handle.data = c;
    c->drain_timer.data = c;
    c->open_handles = 2;
    if (uv_accept(listener, (uv_stream_t *)&c->handle) != 0) {
        teardown(c);
        return;
    }
    uv_tcp_nodelay(&c->handle, 1);
    uv_read_start((uv_stream_t *)&c->handle, on_alloc, on_read);
}

/* --- answering later -------------------------------------------------------------------- */

/*
 * One loop and one thread, so all of this is the wake-up and nothing else: a worker claims the
 * slot from its own thread and uv_async_send brings the answer back here, where the request is.
 * That an async coalesces is why delivery walks the table -- see sc_defer_next_resuming.
 */

static struct conn *conn_of(sc_http_req *req)
{
    return (struct conn *)((char *)req - offsetof(struct conn, req));
}

static void deliver(sc_http_server *server, int32_t slot)
{
    sc_http_req *req = NULL;
    void *work = NULL;
    struct conn *c;
    int keep;

    sc_defer_release(&server->defer, slot, &req, &work);
    server->on_resume(req, work, server->on_resume_data);

    /* NULL is a client that left while the work ran. The connection is already on its way out
     * and the work has been handed back, which is all that was owed. */
    if (req == NULL)
        return;

    c = conn_of(req);
    c->deferred = 0;
    c->ticket = 0;
    if (!c->req.replied) {
        /* Same rule as an ordinary handler: taking a request and not answering it leaves a
         * client waiting, and that is worse to find than a 500 in the log. */
        c->req.status = 500;
        c->req.content_type = "text/plain";
        c->req.reply_len = 0;
    }
    keep = wants_keep_alive(&c->req);
    respond(c, c->req.status, c->req.content_type, c->req.reply, c->req.reply_len, keep,
            c->req.extra_headers);
    consume(c, c->deferred_wire_len);
}

static void on_resume_async(uv_async_t *async)
{
    sc_http_server *server = (sc_http_server *)async->data;
    int32_t slot = -1;

    /* Restart the walk each time rather than continue past the slot just delivered: delivering
     * frees the slot, and a resume that arrived while the callback ran may have taken it. */
    while ((slot = sc_defer_next_resuming(&server->defer, 0)) >= 0)
        deliver(server, slot);
}

sc_status sc_http_on_resume(sc_http_server *server, sc_http_resume_fn fn, void *user_data)
{
    if (server == NULL || fn == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    server->on_resume = fn;
    server->on_resume_data = user_data;
    return SC_OK;
}

sc_status sc_http_defer(sc_http_server *server, sc_http_req *req, void *work, sc_http_ticket *out)
{
    struct conn *c;
    sc_http_ticket ticket;

    if (server == NULL || req == NULL || out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (server->on_resume == NULL)
        return SC_ERR_UNAVAILABLE;

    ticket = sc_defer_arm(&server->defer, req, work);
    if (ticket == 0)
        return SC_ERR_QUEUE_FULL;

    c = conn_of(req);
    c->deferred = 1;
    c->ticket = ticket;
    *out = ticket;
    return SC_OK;
}

sc_status sc_http_resume(sc_http_server *server, sc_http_ticket ticket)
{
    int32_t slot;

    if (server == NULL || ticket == 0)
        return SC_ERR_INVALID_ARGUMENT;
    if (sc_defer_loop_of(ticket) != 0)
        return SC_ERR_INVALID_ARGUMENT; /* this backend has one loop and it is loop zero */
    if (!sc_defer_claim(&server->defer, ticket, &slot))
        return SC_ERR_UNAVAILABLE;
    uv_async_send(&server->resume_async);
    return SC_OK;
}

/* --- the sc_http surface ---------------------------------------------------------------- */

sc_http_server *sc_http_server_create(const sc_http_config *cfg)
{
    sc_http_server *server;

    if (cfg == NULL || cfg->host == NULL || cfg->role == NULL)
        return NULL;
    if (strlen(cfg->host) >= sizeof(server->host) || strlen(cfg->role) >= sizeof(server->role))
        return NULL;

    server = (sc_http_server *)calloc(1, sizeof(*server));
    if (server == NULL)
        return NULL;
    /* Its own loop, not uv_default_loop(): two roles run in two threads and a loop belongs to
     * exactly one of them. */
    if (uv_loop_init(&server->loop) != 0) {
        free(server);
        return NULL;
    }
    server->loop_started = 1;
    server->port = cfg->port;
    memcpy(server->host, cfg->host, strlen(cfg->host) + 1);
    memcpy(server->role, cfg->role, strlen(cfg->role) + 1);

    /*
     * One loop, whatever was asked for, and it says so rather than failing.
     *
     * Refusing a thread count that h2o accepts would break the one promise the seam makes --
     * that the same configuration starts both backends -- and this build exists so that the
     * roles are runnable where h2o will not compile, not so that it can carry load. AGENTS.md
     * section 3b: the fallback is not a deployment option.
     */
    if (cfg->threads > 1)
        sc_log_warn(SC_CAT_STARTUP, "server.threads.clamped",
                    "%s was asked for %u threads; this backend serves on one", server->role,
                    (unsigned)cfg->threads);

    sc_defer_table_init(&server->defer, 0);
    if (uv_async_init(&server->loop, &server->resume_async, on_resume_async) != 0) {
        (void)uv_loop_close(&server->loop);
        free(server);
        return NULL;
    }
    server->resume_async.data = server;
    return server;
}

uint16_t sc_http_thread_count(const sc_http_server *server)
{
    return server != NULL ? 1 : 0;
}

static void close_walking_handle(uv_handle_t *handle, void *arg)
{
    (void)arg;
    if (!uv_is_closing(handle))
        uv_close(handle, NULL);
}

void sc_http_server_destroy(sc_http_server *server)
{
    if (server == NULL)
        return;
    if (server->loop_started) {
        /* Close what is left -- the listener, the quit timer, and any connection still holding a
         * handle -- then run the loop until their close callbacks have fired. uv_loop_close
         * answers UV_EBUSY otherwise, and the memory those callbacks free would leak. */
        uv_walk(&server->loop, close_walking_handle, NULL);
        while (uv_run(&server->loop, UV_RUN_DEFAULT) != 0)
            ;
        (void)uv_loop_close(&server->loop);
    }
    free(server);
}

sc_status sc_http_route(sc_http_server *server, const char *path, sc_http_handler_fn fn,
                        void *user_data)
{
    sc_route *route;

    if (server == NULL || path == NULL || fn == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (server->route_count == MAX_ROUTES)
        return SC_ERR_TOO_LONG;
    route = &server->routes[server->route_count++];
    /* The path is borrowed, not copied: routes are registered from string literals at startup,
     * which is the only time this is called. */
    route->path = path;
    route->path_len = strlen(path);
    route->fn = fn;
    route->user_data = user_data;
    return SC_OK;
}

sc_status sc_http_route_default(sc_http_server *server, sc_http_handler_fn fn, void *user_data)
{
    if (server == NULL || fn == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    server->default_fn = fn;
    server->default_data = user_data;
    return SC_OK;
}

sc_status sc_http_before_route(sc_http_server *server, sc_http_handler_fn fn, void *user_data)
{
    if (server == NULL || fn == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    server->before_route = fn;
    server->before_route_data = user_data;
    return SC_OK;
}

sc_status sc_http_listen(sc_http_server *server)
{
    struct sockaddr_in addr;

    if (server == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (uv_ip4_addr(server->host, (int)server->port, &addr) != 0) {
        sc_log_error(SC_CAT_STARTUP, "server.listen.host_invalid",
                     "%s cannot listen on %s: not an IPv4 address", server->role, server->host);
        return SC_ERR_INVALID_ARGUMENT;
    }
    if (uv_tcp_init(&server->loop, &server->listener) != 0)
        return SC_ERR_NETWORK;
    server->listener.data = server;
    if (uv_tcp_bind(&server->listener, (const struct sockaddr *)&addr, 0) != 0 ||
        uv_listen((uv_stream_t *)&server->listener, 128, on_connection) != 0) {
        sc_log_error(SC_CAT_STARTUP, "server.listen.failed", "%s cannot listen on %s:%u",
                     server->role, server->host, (unsigned)server->port);
        return SC_ERR_NETWORK;
    }
    server->listening = 1;
    sc_log_info(SC_CAT_STARTUP, "server.listen", "%s listening on http://%s:%u", server->role,
                server->host, (unsigned)server->port);
    return SC_OK;
}

/* libuv has no "run until this flag" -- so a timer looks at the flag and stops the loop. */
static void on_quit_tick(uv_timer_t *timer)
{
    sc_http_server *server = (sc_http_server *)timer->data;
    if (sc_quit_requested(server->quit))
        uv_stop(&server->loop);
}

sc_status sc_http_run(sc_http_server *server, const sc_quit_flag *quit)
{
    if (server == NULL || quit == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    server->quit = quit;
    if (uv_timer_init(&server->loop, &server->quit_timer) != 0)
        return SC_ERR_NETWORK;
    server->quit_timer.data = server;
    uv_timer_start(&server->quit_timer, on_quit_tick, SC_RUNTIME_TICK_MS, SC_RUNTIME_TICK_MS);

    (void)uv_run(&server->loop, UV_RUN_DEFAULT);
    sc_log_info(SC_CAT_STARTUP, "server.stop", "%s stopped", server->role);
    return SC_OK;
}

int sc_http_method_is(const sc_http_req *req, const char *method)
{
    size_t len;

    if (req == NULL || method == NULL)
        return 0;
    len = strlen(method);
    return req->method_len == len && memcmp(req->method, method, len) == 0;
}

const char *sc_http_path(const sc_http_req *req, size_t *len)
{
    const char *query;

    if (req == NULL) {
        if (len != NULL)
            *len = 0;
        return NULL;
    }
    /* Split the query off here as well, so a handler sees the same path the route matched. */
    query = memchr(req->path, '?', req->path_len);
    if (len != NULL)
        *len = query != NULL ? (size_t)(query - req->path) : req->path_len;
    return req->path;
}

const char *sc_http_body(const sc_http_req *req, size_t *len)
{
    if (req == NULL || req->body_len == 0) {
        if (len != NULL)
            *len = 0;
        return NULL;
    }
    if (len != NULL)
        *len = req->body_len;
    return req->body;
}

const char *sc_http_header(const sc_http_req *req, const char *name, size_t *len)
{
    if (req == NULL || name == NULL)
        return NULL;
    return header_of(req, name, len);
}

int sc_http_minor_version(const sc_http_req *req)
{
    return req != NULL && req->minor_version >= 1 ? 1 : 0;
}

sc_status sc_http_header_add(sc_http_req *req, const char *name, const char *value,
                             size_t value_len)
{
    int written;

    if (req == NULL || name == NULL || value == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (req->extra_header_count >= SC_HTTP_RESPONSE_HEADERS_MAX)
        return SC_ERR_TOO_LONG;
    /* Serialised as it is added rather than kept as a list: the answer is written from one
     * buffer, and a header that does not fit has to be refused here where the caller can be
     * told, not later where only the connection would notice. */
    written = snprintf(req->extra_headers + req->extra_headers_len,
                       sizeof(req->extra_headers) - req->extra_headers_len, "%s: %.*s\r\n", name,
                       (int)value_len, value);
    if (written <= 0 ||
        (size_t)written >= sizeof(req->extra_headers) - req->extra_headers_len) {
        req->extra_headers[req->extra_headers_len] = '\0';
        return SC_ERR_TOO_LONG;
    }
    req->extra_headers_len += (size_t)written;
    ++req->extra_header_count;
    return SC_OK;
}

sc_status sc_http_reply(sc_http_req *req, int status, const char *content_type, const char *body,
                        size_t body_len)
{
    if (req == NULL || content_type == NULL || body == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (body_len > sizeof(req->reply))
        return SC_ERR_TOO_LONG;

    /* Copied rather than borrowed: the answer goes out after the handler has returned, and a
     * handler's own buffer is usually a stack array that is gone by then. */
    if (body_len != 0)
        memcpy(req->reply, body, body_len);
    req->reply_len = body_len;
    req->status = status;
    req->content_type = content_type;
    req->replied = 1;
    return SC_OK;
}
