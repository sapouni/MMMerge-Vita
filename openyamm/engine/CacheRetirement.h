#pragma once
#include <array>
#include <algorithm>
#include <cstddef>

namespace OpenYAMM::Engine
{
// Declare before the protecting lock. Extracting nodes preserves the key, control
// block and payload until this batch is destroyed AFTER the lock has released.
template<class Cache, size_t Capacity = 64> struct CacheRetirement
{
    std::array<typename Cache::node_type, Capacity> nodes;
    size_t count = 0;
    bool full() const { return count == Capacity; }
    bool extract(Cache &cache, typename Cache::iterator it)
    {
        if (full()) return false;
        nodes[count++] = cache.extract(it);
        return true;
    }
};

template<class Cache, size_t Capacity>
void retireUnpinnedClips(Cache &cache, size_t &bytes, size_t target,
    CacheRetirement<Cache, Capacity> &retired)
{
    if (bytes <= target || retired.full()) return;
    // Fixed scratch retains the oldest eligible entries. No allocation, map-node
    // deletion or PCM destruction under the audio state lock. O(N log Capacity).
    std::array<typename Cache::iterator, Capacity> candidates;
    size_t count = 0;
    auto older = [](const auto &a, const auto &b) { return a->second->lastUse < b->second->lastUse; };
    const size_t limit = Capacity - retired.count;
    for (auto it = cache.begin(); it != cache.end(); ++it)
    {
        if (it->second.use_count() != 1) continue;
        if (count < limit) { candidates[count++] = it; std::push_heap(candidates.begin(), candidates.begin()+count, older); }
        else if (older(it, candidates[0])) {
            std::pop_heap(candidates.begin(), candidates.begin()+count, older);
            candidates[count-1] = it;
            std::push_heap(candidates.begin(), candidates.begin()+count, older);
        }
    }
    std::sort(candidates.begin(), candidates.begin()+count, older);
    for (size_t i = 0; i < count && bytes > target; ++i)
    {
        auto it = candidates[i];
        const size_t released = it->second->samples.capacity() * sizeof(float);
        retired.extract(cache, it);
        bytes -= released;
    }
}
}
