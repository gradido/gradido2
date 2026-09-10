/*
 * Which CPUs share a last-level cache, and how threads are spread over them.
 *
 * A request's data lives on the loop that parsed it, a connection's on the worker that owns it,
 * and a unit of database work crosses between the two once each way (service_core/db_exec.h).
 * Whether that crossing is cheap depends on where the two threads run: between two cores under
 * one L3 a cache line moves in tens of nanoseconds; across L3s, and across sockets, it costs
 * several times that. So loops and workers are placed in groups, a group being the CPUs one L3
 * serves, and a loop hands its work to the workers of its own group.
 *
 *   one L3 (a desktop, a Raspberry Pi 5)     one group, and pinning changes nothing
 *   EPYC: 8 or 16 cores per L3, many L3s     a group per L3, loops and workers inside each
 *   two sockets                               groups never span them
 *
 * Only CPUs this process is allowed to run on are counted: a container restricted to four CPUs
 * sees the host's whole cache topology in sysfs, and pinning a thread to a CPU outside its
 * allowance is refused.
 *
 * Linux reads /sys/devices/system/cpu. Elsewhere, and wherever that says nothing about an L3,
 * there is one group and nothing is pinned -- a placement that cannot be known is left to the
 * scheduler rather than guessed.
 */
#ifndef SERVICE_CORE_TOPOLOGY_H
#define SERVICE_CORE_TOPOLOGY_H

#include <stdint.h>

#include "service_core/status.h"

/** CPUs a set can name. Past today's largest two-socket machines, 768 hardware threads. */
#define SC_CPU_MAX 1024
/** Groups one topology can hold. */
#define SC_CPU_GROUPS_MAX 256

typedef struct sc_cpu_set {
    uint64_t bits[SC_CPU_MAX / 64];
} sc_cpu_set;

typedef struct sc_topology {
    uint16_t group_count;
    /* Zero when the groups are a guess -- no sysfs, no L3 described -- and pinning would only
     * restrict the scheduler for nothing. */
    int pinnable;
    sc_cpu_set groups[SC_CPU_GROUPS_MAX];
    uint16_t cpus_in[SC_CPU_GROUPS_MAX];
} sc_topology;

void sc_cpu_set_clear(sc_cpu_set *set);
void sc_cpu_set_add(sc_cpu_set *set, uint32_t cpu);
int sc_cpu_set_has(const sc_cpu_set *set, uint32_t cpu);
uint32_t sc_cpu_set_count(const sc_cpu_set *set);
void sc_cpu_set_merge(sc_cpu_set *into, const sc_cpu_set *from);

/**
 * Parses the kernel's list form -- `0-3,8-11` -- into @p out. Answers 0 for text that is not
 * one, or names a CPU past SC_CPU_MAX.
 */
int sc_cpu_list_parse(const char *text, sc_cpu_set *out);

/**
 * Reads this machine's groups, restricted to the CPUs the calling thread may run on. Always
 * answers a usable topology -- at worst one group, not pinnable -- and SC_OK.
 */
sc_status sc_topology_load(sc_topology *out);

/**
 * Builds a topology from @p lists, one kernel list per group, restricted to @p allowed (NULL:
 * everything). What sc_topology_load does with what sysfs said; exposed so the grouping can be
 * tested with the shape of a machine this one is not.
 */
void sc_topology_from_lists(const char *const *lists, uint16_t list_count,
                            const sc_cpu_set *allowed, sc_topology *out);

/**
 * Which group item @p index of @p count goes to, spreading them in proportion to the CPUs each
 * group has, in order. Loops use it, and the executor uses it again to find a loop's group, so
 * both agree without either telling the other.
 */
uint16_t sc_topology_group_of(const sc_topology *topology, uint16_t index, uint16_t count);

/** Pins the calling thread to @p set. A topology that is not pinnable pins nothing; so does a
 *  platform that cannot. Answers SC_OK either way -- placement is a preference, never a reason
 *  to fail a start. */
sc_status sc_topology_pin(const sc_topology *topology, const sc_cpu_set *set);

#endif /* SERVICE_CORE_TOPOLOGY_H */
