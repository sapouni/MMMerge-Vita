#pragma once
#include <array>
#include <cstdint>
#include <cstring>
namespace OpenYAMM::Engine
{
// Bounded retry state. No allocation, including the out-of-memory error path.
class VitaRetryGate
{
    struct Entry { std::array<char,192> name{}; uint64_t deadline = 0; unsigned failures = 0; };
    std::array<Entry,32> m_entries{};
    unsigned m_next = 0;
public:
    bool blocked(const char *name, uint64_t now) const
    {
        if (m_next == 0) { return false; }
        for (const auto &entry : m_entries)
            if (std::strcmp(entry.name.data(), name) == 0 && now < entry.deadline) { return true; }
        return false;
    }
    void clear() { m_entries = {}; m_next = 0; }
    bool begin(const char *name, uint64_t now)
    {
        if (!name || std::strlen(name) >= 192) return true;
        Entry *entry = nullptr;
        for (auto &e : m_entries) if (std::strcmp(e.name.data(), name) == 0) { entry = &e; break; }
        if (entry && now < entry->deadline) return false;
        if (!entry)
        {
            entry = &m_entries[m_next++ % m_entries.size()]; *entry = {};
            std::strcpy(entry->name.data(), name);
        }
        const unsigned shift = entry->failures < 3 ? entry->failures++ : 3;
        entry->deadline = now + (1000000000ull << shift);
        return true;
    }
    void succeeded(const char *name)
    {
        for (auto &e : m_entries) if (std::strcmp(e.name.data(),name) == 0) { e = {}; return; }
    }
};
}
