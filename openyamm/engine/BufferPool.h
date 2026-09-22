#pragma once
#include <array>
#include <algorithm>
#include <cstddef>
#include <mutex>
#include <vector>
#include <utility>

namespace OpenYAMM::Engine
{
// Recycles only returned ownership. Never lends a vector still used by a voice,
// queued result or GPU upload. The lock protects swaps, not resize/free work.
template<class T, size_t Slots = 8> class BufferPool
{
public:
    struct Stats { size_t limit, retained, peak, hits, misses, returned, rejected, maxRequest; };
    explicit BufferPool(size_t limitBytes) : limit(limitBytes) {}
    std::vector<T> take(size_t count)
    {
        std::vector<T> result;
        {
            std::lock_guard lock(mutex);
            maxRequest = std::max(maxRequest, count*sizeof(T));
            size_t best = Slots;
            for (size_t i=0;i<Slots;++i)
                if (buffers[i].capacity() >= count && buffers[i].capacity()
                    && (best == Slots || buffers[i].capacity() < buffers[best].capacity())) best=i;
            if (best != Slots) { bytes -= buffers[best].capacity()*sizeof(T); result.swap(buffers[best]); ++hits; }
            else ++misses;
        }
        result.resize(count);
        return result;
    }
    void give(std::vector<T>&& owned)
    {
        std::vector<T> returned = std::move(owned);
        const size_t capacity = returned.capacity()*sizeof(T);
        if (!capacity) return;
        returned.clear();
        {
            std::lock_guard lock(mutex);
            if (capacity > limit) { ++rejected; return; }
            size_t slot = Slots;
            for (size_t i=0;i<Slots;++i)
                if (!buffers[i].capacity()) { slot=i; break; }
            if (slot == Slots)
                for (size_t i=0;i<Slots;++i)
                    if (buffers[i].capacity() < returned.capacity()
                        && (slot == Slots || buffers[i].capacity() < buffers[slot].capacity())) slot=i;
            if (slot != Slots && bytes - buffers[slot].capacity()*sizeof(T) + capacity <= limit)
            {
                bytes = bytes - buffers[slot].capacity()*sizeof(T) + capacity;
                buffers[slot].swap(returned);
                ++returns;
                peak = std::max(peak, bytes);
            }
            else ++rejected;
        }
        // Any rejected/replaced buffer is destroyed after unlocking.
    }
    size_t retainedBytes() const { std::lock_guard lock(mutex); return bytes; }
    Stats snapshot() const { std::lock_guard lock(mutex); return {limit,bytes,peak,hits,misses,returns,rejected,maxRequest}; }
private:
    mutable std::mutex mutex;
    std::array<std::vector<T>,Slots> buffers;
    size_t limit, bytes=0, hits=0, misses=0, returns=0, rejected=0, peak=0, maxRequest=0;
};
}
