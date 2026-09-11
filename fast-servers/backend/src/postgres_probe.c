/*
 * The PostgreSQL probe. postgres_probe.h is the specification and
 * packages/backend/src/setup/findPostgres.ts is the reference this is held to.
 */
#include "postgres_probe.h"

#include <string.h>

#include <uv.h>

/** Length 8, then the code 80877103 -- 1234 in the high half, 5679 in the low one. */
static const char kSslRequest[8] = {0x00, 0x00, 0x00, 0x08, 0x04, (char)0xd2, 0x16, 0x2f};

/** One call's worth of state. Every handle points back here through its `data`. */
typedef struct probe {
    uv_tcp_t tcp;
    uv_connect_t connect;
    uv_write_t write;
    uv_timer_t timer;
    /** The one byte the answer is; nothing past it is read. */
    char reply;
    int connected;
    int settled;
    bk_postgres_answer answer;
} probe;

/**
 * Records @p answer, unless one was recorded already, and closes both handles.
 *
 * The first answer is the one: a timeout that fires while the reply is on its way, or the
 * cancelled connect that closing the socket delivers, both arrive here afterwards and change
 * nothing. With both handles closed the loop has nothing left, and uv_run returns.
 */
static void settle(probe *p, bk_postgres_answer answer)
{
    if (p->settled)
        return;
    p->settled = 1;
    p->answer = answer;
    uv_timer_stop(&p->timer);
    uv_close((uv_handle_t *)&p->timer, NULL);
    uv_close((uv_handle_t *)&p->tcp, NULL);
}

static void unanswered(probe *p)
{
    settle(p, p->connected ? BK_POSTGRES_OTHER : BK_POSTGRES_NOTHING);
}

static void on_timeout(uv_timer_t *timer)
{
    unanswered((probe *)timer->data);
}

static void on_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
    probe *p = (probe *)handle->data;

    (void)suggested;
    *buf = uv_buf_init(&p->reply, 1);
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    probe *p = (probe *)stream->data;

    (void)buf;
    /* Zero is libuv saying there was nothing to read after all, which is not an answer. */
    if (nread == 0)
        return;
    if (nread < 0) {
        unanswered(p);
        return;
    }
    /* 'S' -- it would switch to TLS now -- or 'N' -- it will go on in plain text. Either one is a
     * server that understood the request, and nothing here goes any further with it. */
    settle(p, p->reply == 'S' || p->reply == 'N' ? BK_POSTGRES_FOUND : BK_POSTGRES_OTHER);
}

static void on_write(uv_write_t *write, int status)
{
    if (status < 0)
        unanswered((probe *)write->data);
}

static void on_connect(uv_connect_t *connect, int status)
{
    probe *p = (probe *)connect->data;
    uv_buf_t request = uv_buf_init((char *)kSslRequest, sizeof(kSslRequest));

    if (status < 0) {
        unanswered(p);
        return;
    }
    p->connected = 1;
    if (uv_write(&p->write, (uv_stream_t *)&p->tcp, &request, 1, on_write) != 0 ||
        uv_read_start((uv_stream_t *)&p->tcp, on_alloc, on_read) != 0)
        unanswered(p);
}

bk_postgres_answer bk_postgres_probe(const char *ip, int port, unsigned timeout_ms)
{
    uv_loop_t loop;
    struct sockaddr_in address;
    probe p;

    if (ip == NULL || port <= 0 || port > 65535 || uv_ip4_addr(ip, port, &address) != 0)
        return BK_POSTGRES_NOTHING;
    if (uv_loop_init(&loop) != 0)
        return BK_POSTGRES_NOTHING;

    memset(&p, 0, sizeof(p));
    p.answer = BK_POSTGRES_NOTHING;
    (void)uv_tcp_init(&loop, &p.tcp);
    (void)uv_timer_init(&loop, &p.timer);
    p.tcp.data = &p;
    p.timer.data = &p;
    p.connect.data = &p;
    p.write.data = &p;

    (void)uv_timer_start(&p.timer, on_timeout, timeout_ms, 0);
    if (uv_tcp_connect(&p.connect, &p.tcp, (const struct sockaddr *)&address, on_connect) != 0)
        settle(&p, BK_POSTGRES_NOTHING);

    (void)uv_run(&loop, UV_RUN_DEFAULT);
    (void)uv_loop_close(&loop);
    return p.answer;
}
