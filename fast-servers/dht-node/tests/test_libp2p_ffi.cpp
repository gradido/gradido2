/*
 * libp2p-ffi as this build links it: the prebuilt object, zig's libunwind in place of libgcc_s,
 * and the libraries its NATIVE_LIBS.txt names. The module's behaviour is tested in its own
 * repository; what is tested here is that it links and runs inside this binary's toolchain --
 * its threads, its unwinder, a real call between two nodes.
 *
 * Built only where a module is linked; the stub has nothing to test.
 *
 * C++ because googletest is; see the note at the top of service-core/tests/test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "libp2p_ffi.h"
}

namespace {

constexpr size_t kBufferBytes = 64 * 1024;

struct Identity {
    uint8_t seed[32];
    lp2p_key key;
    lp2p_key group;
    uint8_t delegation[LP2P_DELEGATION_BYTES];
};

Identity identity(uint8_t node_byte, uint8_t group_byte)
{
    Identity id{};
    uint8_t group_seed[32];
    std::memset(id.seed, node_byte, sizeof(id.seed));
    std::memset(group_seed, group_byte, sizeof(group_seed));
    EXPECT_EQ(lp2p_key_from_seed(id.seed, id.key), LP2P_OK);
    EXPECT_EQ(lp2p_key_from_seed(group_seed, id.group), LP2P_OK);
    EXPECT_EQ(lp2p_delegation_sign(group_seed, id.key, 0, id.delegation), LP2P_OK);
    return id;
}

const char *const kProtocols[] = {"/test/echo/1"};
const char *const kListen[] = {"/ip4/127.0.0.1/tcp/0"};

lp2p *start(const Identity &id)
{
    lp2p_options opt;
    lp2p_options_default(&opt);
    std::memcpy(opt.node_seed, id.seed, sizeof(opt.node_seed));
    opt.delegation = id.delegation;
    opt.delegation_len = sizeof(id.delegation);
    std::memcpy(opt.group, id.group, sizeof(lp2p_key));
    opt.listen_addrs = kListen;
    opt.listen_addr_count = 1;
    opt.dht_protocol = "/test/kad/1";
    opt.rpc_protocols = kProtocols;
    opt.rpc_protocol_count = 1;
    opt.rpc_max_request_bytes = 4096;
    opt.rpc_max_response_bytes = 4096;
    opt.quic = 0;
    opt.autonat = 0;
    opt.dcutr = 0;
    opt.announce.enabled = 0;
    opt.reachability = LP2P_REACH_PUBLIC;
    lp2p *node = nullptr;
    EXPECT_EQ(lp2p_start(&opt, &node), LP2P_OK);
    return node;
}

struct Event {
    lp2p_event header;
    std::vector<uint8_t> data;
};

/* Polls @p node until @p match accepts an event, or the deadline passes. */
template <typename Match>
bool wait_for(lp2p *node, Event &out, Match match, int seconds = 15)
{
    std::vector<uint8_t> buf(kBufferBytes);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const int32_t n = lp2p_poll(node, buf.data(), buf.size(), 100);
        if (n < 0)
            return false;
        size_t offset = 0;
        while (offset + sizeof(lp2p_event) <= static_cast<size_t>(n)) {
            Event ev;
            std::memcpy(&ev.header, buf.data() + offset, sizeof(lp2p_event));
            const uint8_t *data = buf.data() + offset + sizeof(lp2p_event);
            ev.data.assign(data, data + ev.header.data_len);
            if (match(ev)) {
                out = ev;
                return true;
            }
            offset += ev.header.size;
        }
    }
    return false;
}

} // namespace

TEST(Libp2pFfi, IsTheVersionTheHeaderDescribes)
{
    EXPECT_EQ(lp2p_abi_version(), static_cast<uint32_t>(LP2P_ABI_VERSION));
}

TEST(Libp2pFfi, ADelegationVerifiesToItsKeys)
{
    const Identity id = identity(1, 0xa0);
    lp2p_key group{}, node{};
    ASSERT_EQ(lp2p_delegation_verify(id.delegation, sizeof(id.delegation), 0, group, node), LP2P_OK);
    EXPECT_EQ(std::memcmp(group, id.group, sizeof(lp2p_key)), 0);
    EXPECT_EQ(std::memcmp(node, id.key, sizeof(lp2p_key)), 0);
}

TEST(Libp2pFfi, TwoNodesCallEachOtherOnLoopback)
{
    const Identity a_id = identity(1, 0xa0);
    const Identity b_id = identity(2, 0xb0);
    lp2p *a = start(a_id);
    lp2p *b = start(b_id);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    Event ev;
    ASSERT_TRUE(wait_for(a, ev, [](const Event &e) { return e.header.type == LP2P_EV_LISTENING; }));
    const std::string address(ev.data.begin(), ev.data.end());
    ASSERT_EQ(lp2p_add_address(b, a_id.key, address.c_str()), LP2P_OK);

    uint64_t id = 0;
    ASSERT_EQ(lp2p_rpc_request(b, a_id.group, a_id.key, 0,
                               reinterpret_cast<const uint8_t *>("ping"), 4, 10000, &id),
              LP2P_OK);
    ASSERT_TRUE(wait_for(a, ev, [](const Event &e) { return e.header.type == LP2P_EV_RPC_REQUEST; }));
    EXPECT_EQ(std::memcmp(ev.header.group, b_id.group, sizeof(lp2p_key)), 0);
    EXPECT_EQ(std::string(ev.data.begin(), ev.data.end()), "ping");
    ASSERT_EQ(lp2p_rpc_respond(a, ev.header.id, reinterpret_cast<const uint8_t *>("pong"), 4),
              LP2P_OK);

    ASSERT_TRUE(wait_for(b, ev, [id](const Event &e) {
        return e.header.id == id &&
               (e.header.type == LP2P_EV_RPC_RESPONSE || e.header.type == LP2P_EV_RPC_FAILED);
    }));
    EXPECT_EQ(ev.header.type, LP2P_EV_RPC_RESPONSE);
    EXPECT_EQ(std::string(ev.data.begin(), ev.data.end()), "pong");
    EXPECT_EQ(std::memcmp(ev.header.node, a_id.key, sizeof(lp2p_key)), 0);

    EXPECT_EQ(lp2p_shutdown(a), LP2P_OK);
    EXPECT_EQ(lp2p_shutdown(b), LP2P_OK);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
