/*
 * The PostgreSQL probe of the setup command: which of three things a port on loopback is.
 *
 * Against listeners this test starts itself rather than against a PostgreSQL: whether a server
 * is installed on the machine that runs the tests is not something a unit test may depend on,
 * and the question the probe asks is eight bytes and one answer, which a listener can give as
 * well as a server can. The reference path holds the same cases in
 * `packages/backend/src/setup/findPostgres.test.ts`.
 *
 * The listener runs a libuv loop on a thread of its own, because the probe blocks on its own
 * loop until it has an answer -- and because libuv is the platform layer here, which is what lets
 * this test build where the probe builds.
 *
 * C++ because googletest is; see the note at the top of service-core/tests/test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <uv.h>

#include "postgres_probe.h"
}

namespace
{

/* Short, because two of the cases below are waited out on purpose. */
constexpr unsigned kTimeoutMs = 200;

/**
 * A listener on 127.0.0.1 that answers the first eight bytes of a connection with @p reply -- or
 * with nothing at all when it is empty, which is a service that accepts and then waits.
 */
class Listener
{
  public:
    explicit Listener(std::string reply) : reply_(std::move(reply))
    {
        sockaddr_in address{};
        sockaddr_storage bound{};
        int length = sizeof(bound);

        uv_loop_init(&loop_);
        uv_tcp_init(&loop_, &server_);
        uv_async_init(&loop_, &stop_, on_stop);
        server_.data = this;
        stop_.data = this;
        uv_ip4_addr("127.0.0.1", 0, &address);
        uv_tcp_bind(&server_, reinterpret_cast<const sockaddr *>(&address), 0);
        uv_listen(reinterpret_cast<uv_stream_t *>(&server_), 4, on_connection);
        uv_tcp_getsockname(&server_, reinterpret_cast<sockaddr *>(&bound), &length);
        /* Network order, read byte by byte rather than through ntohs, which lives in a different
         * header on every platform this builds for. */
        const auto *port = reinterpret_cast<const unsigned char *>(
            &reinterpret_cast<const sockaddr_in *>(&bound)->sin_port);
        port_ = port[0] << 8 | port[1];
        thread_ = std::thread([this] { uv_run(&loop_, UV_RUN_DEFAULT); });
    }

    ~Listener()
    {
        uv_async_send(&stop_);
        thread_.join();
        uv_loop_close(&loop_);
    }

    Listener(const Listener &) = delete;
    Listener &operator=(const Listener &) = delete;

    int port() const
    {
        return port_;
    }

    /** What the probe sent. Under the lock, because the loop's thread is still running. */
    std::vector<unsigned char> received() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }

  private:
    static void on_connection(uv_stream_t *server, int status)
    {
        auto *self = static_cast<Listener *>(server->data);
        if (status < 0 || self->accepted_)
            return;
        self->accepted_ = true;
        uv_tcp_init(&self->loop_, &self->client_);
        self->client_.data = self;
        if (uv_accept(server, reinterpret_cast<uv_stream_t *>(&self->client_)) == 0)
            uv_read_start(reinterpret_cast<uv_stream_t *>(&self->client_), on_alloc, on_read);
    }

    static void on_alloc(uv_handle_t *handle, size_t, uv_buf_t *buf)
    {
        auto *self = static_cast<Listener *>(handle->data);
        *buf = uv_buf_init(self->buffer_, sizeof(self->buffer_));
    }

    static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
    {
        auto *self = static_cast<Listener *>(stream->data);
        if (nread <= 0)
            return;
        size_t received;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->received_.insert(self->received_.end(), buf->base, buf->base + nread);
            received = self->received_.size();
        }
        if (received >= 8 && !self->reply_.empty() && !self->replied_) {
            self->replied_ = true;
            uv_buf_t reply = uv_buf_init(self->reply_.data(), self->reply_.size());
            uv_write(&self->write_, stream, &reply, 1, nullptr);
        }
    }

    static void on_stop(uv_async_t *async)
    {
        uv_walk(
            async->loop,
            [](uv_handle_t *handle, void *) {
                if (!uv_is_closing(handle))
                    uv_close(handle, nullptr);
            },
            nullptr);
    }

    std::string reply_;
    uv_loop_t loop_{};
    uv_tcp_t server_{};
    uv_tcp_t client_{};
    uv_async_t stop_{};
    uv_write_t write_{};
    char buffer_[64]{};
    bool accepted_ = false;
    bool replied_ = false;
    mutable std::mutex mutex_;
    std::vector<unsigned char> received_;
    int port_ = 0;
    std::thread thread_;
};

/** A port nothing listens on: one that was just listened on and let go. */
int closed_port()
{
    int port;
    {
        Listener listener("");
        port = listener.port();
    }
    return port;
}

TEST(PostgresProbe, AServerThatAnswersNIsPostgres)
{
    Listener listener("N");
    EXPECT_EQ(bk_postgres_probe("127.0.0.1", listener.port(), kTimeoutMs), BK_POSTGRES_FOUND);
}

TEST(PostgresProbe, AServerThatAnswersSIsPostgres)
{
    Listener listener("S");
    EXPECT_EQ(bk_postgres_probe("127.0.0.1", listener.port(), kTimeoutMs), BK_POSTGRES_FOUND);
}

TEST(PostgresProbe, SendsTheSslRequestAndNothingBeforeIt)
{
    Listener listener("N");
    bk_postgres_probe("127.0.0.1", listener.port(), kTimeoutMs);

    EXPECT_EQ(listener.received(),
              (std::vector<unsigned char>{0x00, 0x00, 0x00, 0x08, 0x04, 0xd2, 0x16, 0x2f}));
}

TEST(PostgresProbe, SomethingThatAnswersWithAnythingElseIsNotPostgres)
{
    Listener listener("HTTP/1.1 400 Bad Request\r\n\r\n");
    EXPECT_EQ(bk_postgres_probe("127.0.0.1", listener.port(), kTimeoutMs), BK_POSTGRES_OTHER);
}

TEST(PostgresProbe, SomethingThatAcceptsAndSaysNothingIsNotPostgresEither)
{
    Listener listener("");
    EXPECT_EQ(bk_postgres_probe("127.0.0.1", listener.port(), kTimeoutMs), BK_POSTGRES_OTHER);
}

TEST(PostgresProbe, APortNothingListensOnIsNothing)
{
    EXPECT_EQ(bk_postgres_probe("127.0.0.1", closed_port(), kTimeoutMs), BK_POSTGRES_NOTHING);
}

TEST(PostgresProbe, AnAddressOrPortThatIsNoneIsNothing)
{
    EXPECT_EQ(bk_postgres_probe(nullptr, 5432, kTimeoutMs), BK_POSTGRES_NOTHING);
    EXPECT_EQ(bk_postgres_probe("not an address", 5432, kTimeoutMs), BK_POSTGRES_NOTHING);
    EXPECT_EQ(bk_postgres_probe("127.0.0.1", 0, kTimeoutMs), BK_POSTGRES_NOTHING);
    EXPECT_EQ(bk_postgres_probe("127.0.0.1", 65536, kTimeoutMs), BK_POSTGRES_NOTHING);
}

} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
