#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace OpenYAMM::Game
{
struct VitaLosCache
{
    std::vector<int8_t> values;
    size_t actorCount = 0;
    bool enabled = false;
    uint32_t hits = 0, misses = 0, results = 0, invalidations = 0;
    void begin(size_t count)
    {
        actorCount = count;
        hits = misses = results = invalidations = 0;
        values.assign((count + 1) * (count + 1), -1);
        enabled = true;
    }
    void invalidate()
    {
        std::fill(values.begin(), values.end(), int8_t(-1));
        if (enabled) { ++invalidations; }
    }
    template<class Query> bool queryPair(size_t first, size_t second, Query calculate)
    {
        if (!enabled) { return calculate(); }
        // actorCount is the party sentinel; anonymous/world rays do not use this cache.
        if (first == size_t(-1)) { first = actorCount; }
        if (second == size_t(-1)) { second = actorCount; }
        if (first > actorCount || second > actorCount) { return calculate(); }
        if (second < first) { std::swap(first, second); }
#if defined(OPENYAMM_VITA_LOADER) || defined(OPENYAMM_VITA_LOS_CONCURRENCY_TEST)
        int8_t *slot = &values[first * (actorCount + 1) + second];
        const int8_t value = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
        if (value >= 0) { __atomic_fetch_add(&hits, 1u, __ATOMIC_RELAXED); return value != 0; }
        __atomic_fetch_add(&misses, 1u, __ATOMIC_RELAXED);
        const bool result = calculate();
        int8_t expected = -1;
        if (__atomic_compare_exchange_n(slot, &expected, int8_t(result ? 1 : 0), false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            __atomic_fetch_add(&results, 1u, __ATOMIC_RELAXED);
            return result;
        }
        return expected != 0;
#else
        int8_t &value = values[first * (actorCount + 1) + second];
        if (value >= 0) { ++hits; return value != 0; }
        ++misses;
        const bool result = calculate();
        value = result ? 1 : 0;
        ++results;
        return result;
#endif
    }
};
}
