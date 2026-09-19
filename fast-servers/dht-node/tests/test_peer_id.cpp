// Peer ids as js-libp2p writes them: the expected texts are peerIdFromPublicKey on the reference
// path, for the same keys.
#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include "peer_id.h"
}

namespace {

struct Vector {
    unsigned char fill;
    const char *text;
};

const Vector kVectors[] = {
    {0x00, "12D3KooW9pNAk8aiBuGVQtWRdbkLmo5qVL3e2h5UxbN2Nz9ttwiw"},
    {0x01, "12D3KooW9tHTtS3inCZiYykw4u5G4frbjVFqhkmJX12gSNCVeH3e"},
    {0x7f, "12D3KooWJQ4hvNj1HmzG8PGt2YB2iy6iVVmCZmGcvR1MnFqynFgi"},
    {0xff, "12D3KooWT3gYEvLJyx1FyyHqrmvdy1tMjpgxmu9aSeKMEuafQtyC"},
};

TEST(PeerId, FormatsWhatJsLibp2pWrites)
{
    for (const Vector &v : kVectors) {
        uint8_t key[32];
        char text[DHT_PEER_ID_TEXT_MAX];
        std::memset(key, v.fill, sizeof(key));
        dht_peer_id_format(key, text);
        EXPECT_STREQ(text, v.text);
    }
}

TEST(PeerId, ParsesItBack)
{
    for (const Vector &v : kVectors) {
        uint8_t key[32];
        uint8_t expected[32];
        std::memset(expected, v.fill, sizeof(expected));
        ASSERT_TRUE(dht_peer_id_parse(v.text, std::strlen(v.text), key));
        EXPECT_EQ(0, std::memcmp(key, expected, sizeof(key)));
    }
}

TEST(PeerId, RefusesWhatIsNoEd25519PeerId)
{
    uint8_t key[32];
    const std::string good = kVectors[1].text;

    EXPECT_FALSE(dht_peer_id_parse("", 0, key));
    EXPECT_FALSE(dht_peer_id_parse(good.c_str(), good.size() - 1, key));
    EXPECT_FALSE(dht_peer_id_parse((good + "1").c_str(), good.size() + 1, key));
    std::string changed = good;
    changed[10] = '0'; // not in the alphabet
    EXPECT_FALSE(dht_peer_id_parse(changed.c_str(), changed.size(), key));
    // A secp256k1 or RSA peer id: base58, but not the ed25519 prefix.
    const char *other = "QmYyQSo1c1Ym7orWxLYvCrM2EmxFTANf8wXmmE7DWjhx5N";
    EXPECT_FALSE(dht_peer_id_parse(other, std::strlen(other), key));
}

} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
