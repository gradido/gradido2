/*
 * Cache groups, read from sysfs and restricted to what this process may run on. See
 * service_core/topology.h for why they matter.
 */
#include "service_core/topology.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

void sc_cpu_set_clear(sc_cpu_set *set)
{
    memset(set, 0, sizeof(*set));
}

void sc_cpu_set_add(sc_cpu_set *set, uint32_t cpu)
{
    if (cpu < SC_CPU_MAX)
        set->bits[cpu / 64] |= (uint64_t)1 << (cpu % 64);
}

int sc_cpu_set_has(const sc_cpu_set *set, uint32_t cpu)
{
    return cpu < SC_CPU_MAX && (set->bits[cpu / 64] >> (cpu % 64) & 1u) != 0;
}

uint32_t sc_cpu_set_count(const sc_cpu_set *set)
{
    uint32_t count = 0;
    size_t i;

    for (i = 0; i != SC_CPU_MAX / 64; ++i) {
        uint64_t word = set->bits[i];

        while (word != 0) {
            word &= word - 1;
            ++count;
        }
    }
    return count;
}

void sc_cpu_set_merge(sc_cpu_set *into, const sc_cpu_set *from)
{
    size_t i;

    for (i = 0; i != SC_CPU_MAX / 64; ++i)
        into->bits[i] |= from->bits[i];
}

static int sets_equal(const sc_cpu_set *a, const sc_cpu_set *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

int sc_cpu_list_parse(const char *text, sc_cpu_set *out)
{
    const char *p = text;

    sc_cpu_set_clear(out);
    if (text == NULL)
        return 0;
    while (*p != '\0' && *p != '\n') {
        char *end;
        unsigned long first = strtoul(p, &end, 10);
        unsigned long last = first;
        unsigned long cpu;

        if (end == p)
            return 0;
        p = end;
        if (*p == '-') {
            last = strtoul(p + 1, &end, 10);
            if (end == p + 1 || last < first)
                return 0;
            p = end;
        }
        if (last >= SC_CPU_MAX)
            return 0;
        for (cpu = first; cpu <= last; ++cpu)
            sc_cpu_set_add(out, (uint32_t)cpu);
        if (*p == ',')
            ++p;
        else if (*p != '\0' && *p != '\n')
            return 0;
    }
    return 1;
}

void sc_topology_from_lists(const char *const *lists, uint16_t list_count,
                            const sc_cpu_set *allowed, sc_topology *out)
{
    uint16_t i;

    memset(out, 0, sizeof(*out));
    for (i = 0; i != list_count && out->group_count != SC_CPU_GROUPS_MAX; ++i) {
        sc_cpu_set set;
        uint16_t g;
        size_t w;

        if (!sc_cpu_list_parse(lists[i], &set))
            continue;
        if (allowed != NULL)
            for (w = 0; w != SC_CPU_MAX / 64; ++w)
                set.bits[w] &= allowed->bits[w];
        if (sc_cpu_set_count(&set) == 0)
            continue;
        for (g = 0; g != out->group_count && !sets_equal(&out->groups[g], &set); ++g)
            ;
        if (g != out->group_count)
            continue; /* every CPU of one L3 names the same list */
        out->groups[out->group_count] = set;
        out->cpus_in[out->group_count] = (uint16_t)sc_cpu_set_count(&set);
        ++out->group_count;
    }
    out->pinnable = out->group_count != 0;
}

uint16_t sc_topology_group_of(const sc_topology *topology, uint16_t index, uint16_t count)
{
    uint32_t total = 0;
    uint64_t position;
    uint32_t seen = 0;
    uint16_t g;

    if (topology == NULL || topology->group_count <= 1 || count == 0)
        return 0;
    for (g = 0; g != topology->group_count; ++g)
        total += topology->cpus_in[g];
    /* The middle of the item's share of all CPUs, in order: item 0 of 4 on 16 CPUs sits at CPU
     * 2, item 1 at 6. Whichever group holds that CPU gets the item -- proportional, and the
     * same answer every time it is asked. */
    position = ((uint64_t)(2u * index + 1u) * total) / (2u * count);
    for (g = 0; g != topology->group_count; ++g) {
        seen += topology->cpus_in[g];
        if (position < seen)
            return g;
    }
    return (uint16_t)(topology->group_count - 1);
}

/* The CPUs the calling thread may run on, or every CPU where that cannot be asked. */
static void allowed_cpus(sc_cpu_set *out)
{
    int size = uv_cpumask_size();
    uv_thread_t self = uv_thread_self();
    char *mask;
    int cpu;

    sc_cpu_set_clear(out);
    mask = size > 0 ? (char *)calloc((size_t)size, 1) : NULL;
    if (mask == NULL || uv_thread_getaffinity(&self, mask, (size_t)size) != 0) {
        unsigned int n = uv_available_parallelism();

        free(mask);
        for (cpu = 0; cpu < (int)n && cpu < SC_CPU_MAX; ++cpu)
            sc_cpu_set_add(out, (uint32_t)cpu);
        return;
    }
    for (cpu = 0; cpu < size && cpu < SC_CPU_MAX; ++cpu)
        if (mask[cpu])
            sc_cpu_set_add(out, (uint32_t)cpu);
    free(mask);
}

/* Reads one small sysfs file into @p out, without its newline. */
static int read_line(const char *path, char *out, size_t out_size)
{
    FILE *file = fopen(path, "r");
    size_t length;

    if (file == NULL)
        return 0;
    if (fgets(out, (int)out_size, file) == NULL) {
        fclose(file);
        return 0;
    }
    fclose(file);
    length = strlen(out);
    while (length > 0 && (out[length - 1] == '\n' || out[length - 1] == '\r'))
        out[--length] = '\0';
    return 1;
}

/* The list of CPUs that share @p cpu's L3, from whichever cache index says it is level 3. */
static int l3_list_of(uint32_t cpu, char *out, size_t out_size)
{
    int index;

    for (index = 0; index != 10; ++index) {
        char path[128];
        char level[8];

        (void)snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%d/level",
                       (unsigned)cpu, index);
        if (!read_line(path, level, sizeof(level)))
            continue;
        if (strcmp(level, "3") != 0)
            continue;
        (void)snprintf(path, sizeof(path),
                       "/sys/devices/system/cpu/cpu%u/cache/index%d/shared_cpu_list", (unsigned)cpu,
                       index);
        return read_line(path, out, out_size);
    }
    return 0;
}

sc_status sc_topology_load(sc_topology *out)
{
    /* Heap rather than static: two roles in one process may each load a topology at startup,
     * at the same time. */
    char (*lists)[256] = (char (*)[256])calloc(SC_CPU_GROUPS_MAX, 256);
    const char *pointers[SC_CPU_GROUPS_MAX];
    sc_cpu_set allowed;
    uint16_t list_count = 0;
    uint32_t cpu;

    if (out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    allowed_cpus(&allowed);

    for (cpu = 0; lists != NULL && cpu != SC_CPU_MAX && list_count != SC_CPU_GROUPS_MAX; ++cpu) {
        uint16_t i;

        if (!sc_cpu_set_has(&allowed, cpu))
            continue;
        if (!l3_list_of(cpu, lists[list_count], sizeof(lists[list_count])))
            continue;
        for (i = 0; i != list_count && strcmp(lists[i], lists[list_count]) != 0; ++i)
            ;
        if (i == list_count) {
            pointers[list_count] = lists[list_count];
            ++list_count;
        }
    }
    sc_topology_from_lists(pointers, list_count, &allowed, out);
    free(lists);

    /* Nothing said about an L3 -- not Linux, or a CPU that has none -- is one group of every
     * CPU allowed, and nothing is pinned to it. */
    if (out->group_count == 0) {
        out->group_count = 1;
        out->groups[0] = allowed;
        out->cpus_in[0] = (uint16_t)sc_cpu_set_count(&allowed);
        out->pinnable = 0;
    }
    /* One group pins nothing worth pinning: every thread may already run on every CPU in it. */
    if (out->group_count == 1)
        out->pinnable = 0;
    return SC_OK;
}

sc_status sc_topology_pin(const sc_topology *topology, const sc_cpu_set *set)
{
    int size;
    uv_thread_t self;
    char *mask;
    int cpu;

    if (topology == NULL || set == NULL || !topology->pinnable)
        return SC_OK;
    size = uv_cpumask_size();
    if (size <= 0 || (mask = (char *)calloc((size_t)size, 1)) == NULL)
        return SC_OK;
    for (cpu = 0; cpu < size && cpu < SC_CPU_MAX; ++cpu)
        mask[cpu] = (char)sc_cpu_set_has(set, (uint32_t)cpu);
    self = uv_thread_self();
    (void)uv_thread_setaffinity(&self, mask, NULL, (size_t)size);
    free(mask);
    return SC_OK;
}
