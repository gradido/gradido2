/*
 * Cache groups: reading the kernel's CPU lists, keeping only what this process may run on, and
 * spreading threads over groups in proportion to their CPUs.
 *
 * Most of it is tested on machines this one is not -- two L3s, an uneven pair, a container
 * restricted to part of the host -- because the one this runs on has a single L3, and a single
 * group is exactly the case where none of it shows.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <vector>

extern "C" {
#include "service_core/topology.h"
}

namespace
{

TEST(CpuList, ReadsTheKernelsListForm)
{
    sc_cpu_set set;

    ASSERT_TRUE(sc_cpu_list_parse("0-3,8-11", &set));
    EXPECT_EQ(sc_cpu_set_count(&set), 8u);
    EXPECT_TRUE(sc_cpu_set_has(&set, 3));
    EXPECT_FALSE(sc_cpu_set_has(&set, 4));
    EXPECT_TRUE(sc_cpu_set_has(&set, 11));

    ASSERT_TRUE(sc_cpu_list_parse("5\n", &set));
    EXPECT_EQ(sc_cpu_set_count(&set), 1u);
}

TEST(CpuList, RefusesWhatIsNotOne)
{
    sc_cpu_set set;

    EXPECT_FALSE(sc_cpu_list_parse("a-b", &set));
    EXPECT_FALSE(sc_cpu_list_parse("3-1", &set));
    EXPECT_FALSE(sc_cpu_list_parse("0-4096", &set));
    EXPECT_FALSE(sc_cpu_list_parse("1;2", &set));
}

/* Every CPU under one L3 reports the same list; one group per L3, not per CPU. And a container
 * allowed only part of the host gets groups of only those CPUs -- the rest of an L3 it cannot
 * run on is not somewhere a thread may be pinned. */
TEST(Topology, OneGroupPerCacheAndOnlyTheAllowedCpus)
{
    const char *lists[] = {"0-7", "0-7", "8-15", "8-15", "16-23"};
    sc_cpu_set allowed;
    sc_topology topology;

    ASSERT_TRUE(sc_cpu_list_parse("0-3,8-9", &allowed));
    sc_topology_from_lists(lists, 5, &allowed, &topology);

    ASSERT_EQ(topology.group_count, 2);
    EXPECT_EQ(topology.cpus_in[0], 4);
    EXPECT_EQ(topology.cpus_in[1], 2);
    EXPECT_TRUE(topology.pinnable);
}

std::vector<uint16_t> spread(const sc_topology &topology, uint16_t count)
{
    std::vector<uint16_t> groups;

    for (uint16_t i = 0; i != count; ++i)
        groups.push_back(sc_topology_group_of(&topology, i, count));
    return groups;
}

/* Proportional to the CPUs, in order, and the same answer for the same question -- which is
 * what lets the executor find a loop's group without being told. */
TEST(Topology, SpreadsInProportionToTheCpus)
{
    const char *even[] = {"0-7", "8-15"};
    const char *uneven[] = {"0-11", "12-15"};
    sc_topology topology;

    sc_topology_from_lists(even, 2, nullptr, &topology);
    EXPECT_EQ(spread(topology, 4), (std::vector<uint16_t>{0, 0, 1, 1}));
    EXPECT_EQ(spread(topology, 1), (std::vector<uint16_t>{1}));
    EXPECT_EQ(spread(topology, 16).front(), 0);
    EXPECT_EQ(spread(topology, 16).back(), 1);

    sc_topology_from_lists(uneven, 2, nullptr, &topology);
    EXPECT_EQ(spread(topology, 4), (std::vector<uint16_t>{0, 0, 0, 1}));
    EXPECT_EQ(sc_topology_group_of(&topology, 0, 0), 0);
}

/* This machine, whatever it is: a usable answer, and every allowed CPU in exactly one group. */
TEST(Topology, LoadsThisMachine)
{
    sc_topology topology;
    uint32_t total = 0;

    ASSERT_EQ(sc_topology_load(&topology), SC_OK);
    ASSERT_GE(topology.group_count, 1);
    for (uint16_t g = 0; g != topology.group_count; ++g) {
        EXPECT_GT(topology.cpus_in[g], 0);
        total += topology.cpus_in[g];
    }
    EXPECT_GT(total, 0u);
    /* One group pins nothing: every thread may already run on every CPU in it. */
    if (topology.group_count == 1)
        EXPECT_FALSE(topology.pinnable);
    EXPECT_EQ(sc_topology_pin(&topology, &topology.groups[0]), SC_OK);
}

} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
