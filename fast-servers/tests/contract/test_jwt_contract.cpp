/*
 * `contracts/test-vectors/jwt.json`, run against jwt.c.
 *
 * The other half of this suite is `packages/contract-tests/src/jwt.test.ts`, which runs the same
 * file against jose. Neither half is the authority: the file is, and each implementation is
 * measured against it rather than against the other. That is what makes a disagreement show up
 * as one named failure instead of as two suites that are both green.
 *
 * What is contracted is `expect`: whether the token is accepted, and the claim value that comes
 * out of it. `c.result` is asserted here as well, because it is C's own vocabulary and a log
 * line carries it -- but it is not contracted across the two implementations, and the file says
 * why: every refusal is one 401 with one body, so a caller cannot observe which one it was, and
 * the two check in a different order.
 *
 * A vector with a `typescript` block is one the TypeScript path answers differently. This runner
 * ignores that block -- `expect` is what jwt.c is held to either way -- and only counts them, so
 * that a green suite here and a red one over there still names the gap.
 *
 * This binary is not one of the component unit tests. It sits under `tests/` rather than beside
 * service-core because it needs arnm's header to read the vector file, and a component test
 * deliberately sees nothing but the component it tests. `service-core/tests/test_jwt.cpp` is the
 * unit test for jwt.c, and it is where everything the contract cannot express lives: null
 * arguments, buffer bounds, the sign-then-verify round trip.
 *
 * C++ because googletest is; see the note at the top of service-core/tests/test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "service_core/jwt.h"
}

#include "vectors.hpp"

namespace
{

/** The vector file, opened once: parsing it per vector would be 37 parses of the same bytes. */
const contract::VectorFile &vectorFile()
{
    static const contract::VectorFile file("jwt");
    return file;
}

/** `c.result` as the file spells it, which is how `service_core/jwt.h` spells it. */
sc_jwt_result resultFromName(const std::string &name)
{
    static const std::map<std::string, sc_jwt_result> byName = {
        {"SC_JWT_OK", SC_JWT_OK},
        {"SC_JWT_MALFORMED", SC_JWT_MALFORMED},
        {"SC_JWT_BAD_ALGORITHM", SC_JWT_BAD_ALGORITHM},
        {"SC_JWT_BAD_SIGNATURE", SC_JWT_BAD_SIGNATURE},
        {"SC_JWT_EXPIRED", SC_JWT_EXPIRED},
        {"SC_JWT_BAD_ISSUER", SC_JWT_BAD_ISSUER},
        {"SC_JWT_BAD_AUDIENCE", SC_JWT_BAD_AUDIENCE},
        {"SC_JWT_MISSING_CLAIM", SC_JWT_MISSING_CLAIM},
    };
    const auto found = byName.find(name);
    if (found == byName.end())
        throw std::runtime_error("no such sc_jwt_result: " + name);
    return found->second;
}

const char *nameOf(sc_jwt_result result)
{
    switch (result) {
    case SC_JWT_OK: return "SC_JWT_OK";
    case SC_JWT_MALFORMED: return "SC_JWT_MALFORMED";
    case SC_JWT_BAD_ALGORITHM: return "SC_JWT_BAD_ALGORITHM";
    case SC_JWT_BAD_SIGNATURE: return "SC_JWT_BAD_SIGNATURE";
    case SC_JWT_EXPIRED: return "SC_JWT_EXPIRED";
    case SC_JWT_BAD_ISSUER: return "SC_JWT_BAD_ISSUER";
    case SC_JWT_BAD_AUDIENCE: return "SC_JWT_BAD_AUDIENCE";
    case SC_JWT_MISSING_CLAIM: return "SC_JWT_MISSING_CLAIM";
    }
    return "?";
}

/** One vector, in the shape this runner needs it. */
struct JwtVector {
    std::string id;
    std::string why;
    std::string claimName;
    std::optional<std::string> issuer;
    std::optional<std::string> audience;
    int64_t now = 0;
    std::string token;
    bool expectAccepted = false;
    std::optional<std::string> expectClaim;
    sc_jwt_result expectResult = SC_JWT_OK;
    bool typescriptDiverges = false;
};

/*
 * The always-present members go in one table; the nullable ones are asked for one at a time.
 *
 * A table converts a member where it stands, and a `null` stops the walk -- so a single table
 * over a whole vector would stop at the first null in it, usually `issuer`, and read nothing
 * after. vectors.hpp has the long form of that reasoning. The table is in the file's own member
 * order, which is the order the walk meets the chain in.
 */
JwtVector readVector(arnm_json_value *value)
{
    JwtVector vector;
    arnm_memory_block id {}, why {}, claimName {}, token {};
    arnm_json_value *expect = nullptr;
    arnm_json_value *reported = nullptr;
    arnm_json_field fields[] = {
        ARNM_JSON_FIELD_STRING("id", &id),
        ARNM_JSON_FIELD_STRING("why", &why),
        ARNM_JSON_FIELD_STRING("claimName", &claimName),
        ARNM_JSON_FIELD_STRING("token", &token),
        ARNM_JSON_FIELD_VALUE("expect", &expect),
        ARNM_JSON_FIELD_VALUE("c", &reported),
    };
    uint64_t found = 0;

    arnm_json_read_object(value, fields, 6, &found);
    if (found != 0x3fu)
        throw std::runtime_error("a vector is missing one of id, why, claimName, token, expect, c");

    vector.id.assign(reinterpret_cast<const char *>(id.data), id.size);
    vector.why.assign(reinterpret_cast<const char *>(why.data), why.size);
    vector.claimName.assign(reinterpret_cast<const char *>(claimName.data), claimName.size);
    vector.token.assign(reinterpret_cast<const char *>(token.data), token.size);

    vector.issuer = contract::nullableString(value, "issuer");
    vector.audience = contract::nullableString(value, "audience");
    vector.now = contract::requiredDecimal(value, "now");
    /* `why` is the witness: a divergence without a reason is an excuse, so the block always
     * carries one -- and arnm cannot otherwise tell an object from the `null` that stands in its
     * place. vectors.hpp, objectWith(), has the long form. */
    vector.typescriptDiverges = contract::objectWith(value, "typescript", "why") != nullptr;

    vector.expectAccepted = contract::requiredBool(expect, "accepted");
    vector.expectClaim = contract::nullableString(expect, "claim");
    vector.expectResult = resultFromName(contract::requiredString(reported, "result"));

    /* A vector may not disagree with itself. A refusal that names a claim value, or an
     * acceptance that names none, would be read one way here and another way by a runner that
     * only looks at `accepted` -- and the two suites would then be green about different
     * things. */
    if (vector.expectAccepted != vector.expectClaim.has_value())
        throw std::runtime_error(vector.id + ": expect.accepted and expect.claim disagree");
    if (vector.expectAccepted != (vector.expectResult == SC_JWT_OK))
        throw std::runtime_error(vector.id + ": expect.accepted and c.result disagree");

    return vector;
}

std::vector<JwtVector> allVectors()
{
    std::vector<JwtVector> vectors;
    vectors.reserve(vectorFile().vectors().size());
    for (arnm_json_value *value : vectorFile().vectors())
        vectors.push_back(readVector(value));
    return vectors;
}

/** The secret every vector in the file is signed with. */
std::string secret()
{
    return contract::contractValue(vectorFile().root(), "secret");
}

class JwtContract : public testing::Test
{
};

/*
 * The bounds in `rules` are behaviour both implementations share, and one of them is a buffer in
 * this one. A reference implementation that issued a longer identifier than the fast path can
 * copy out would mint tokens the fast path refuses, which is why the number is in the contract
 * at all -- and why it is checked against the header here rather than trusted.
 */
TEST_F(JwtContract, TheBoundsInTheFileAreTheOnesThisImplementationHas)
{
    arnm_json_value *rules = contract::requiredObject(vectorFile().root(), "rules");

    EXPECT_EQ(contract::contractValue(rules, "claimMaxBytes"),
              std::to_string(SC_JWT_MAX_CLAIM - 1))
        << "rules.claimMaxBytes and SC_JWT_MAX_CLAIM in service_core/jwt.h have drifted apart";
    EXPECT_EQ(contract::contractValue(rules, "algorithm"), "HS256");
    EXPECT_EQ(contract::contractValue(rules, "expiryIsClosed"), "true");
}

/*
 * Every vector, in one test rather than one test each: googletest builds a parameter list at
 * static-initialisation time, and a vector file that cannot be read would then fail before
 * anything can say why. EXPECT rather than ASSERT, so one bad vector does not hide the rest, and
 * every failure names its id -- which is what the id is for.
 */
TEST_F(JwtContract, EveryVector)
{
    const std::vector<JwtVector> vectors = allVectors();
    const std::string key = secret();
    ASSERT_FALSE(vectors.empty());

    for (const JwtVector &vector : vectors) {
        sc_jwt_config config {};
        config.secret = reinterpret_cast<const uint8_t *>(key.data());
        config.secret_len = key.size();
        config.issuer = vector.issuer.has_value() ? vector.issuer->c_str() : nullptr;
        config.audience = vector.audience.has_value() ? vector.audience->c_str() : nullptr;

        /* Poisoned, not zeroed: a verifier that returns a refusal must not have written here,
         * and a zeroed buffer would let a forgotten write pass as an empty claim. */
        char out[SC_JWT_MAX_CLAIM];
        std::memset(out, '?', sizeof(out));

        const sc_jwt_result result =
            sc_jwt_verify_hs256(&config, vector.token.c_str(), vector.token.size(),
                                vector.claimName.c_str(), out, vector.now);

        SCOPED_TRACE(vector.id + "\n  " + vector.why);
        EXPECT_EQ(result == SC_JWT_OK, vector.expectAccepted)
            << "answered " << nameOf(result) << ", the contract says "
            << (vector.expectAccepted ? "accepted" : "refused");
        EXPECT_EQ(result, vector.expectResult)
            << "answered " << nameOf(result) << ", this implementation promises "
            << nameOf(vector.expectResult);
        if (result == SC_JWT_OK && vector.expectClaim.has_value())
            EXPECT_EQ(std::string(out), *vector.expectClaim);
    }
}

/*
 * The vectors where the two implementations do not agree, named rather than counted, so that
 * closing a gap is a change to this list and to nothing else. Every entry is a token the two
 * answer differently, and the list is meant to shrink.
 *
 * `packages/contract-tests/src/jwt.test.ts` asserts the same list from its side, and
 * additionally that each divergence really still happens -- the assertion this side cannot make,
 * because it is jose that diverges and not jwt.c.
 */
TEST_F(JwtContract, TheKnownDivergencesAreTheOnesTheFileDeclares)
{
    std::vector<std::string> diverging;
    for (const JwtVector &vector : allVectors())
        if (vector.typescriptDiverges)
            diverging.push_back(vector.id);

    EXPECT_EQ(diverging, std::vector<std::string> {"jwt.verify.refuses-a-list-valued-audience"});
}

} // namespace

int main(int argc, char **argv)
{
    sc_jwt_init();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
