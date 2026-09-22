#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenYAMM::Engine
{
// Immutable mount metadata only; never caches file contents or mutable directory listings.
class VitaDirectoryCache
{
public:
    using Names = std::vector<std::string>;
    using Listing = std::shared_ptr<const Names>;
    static int compareFolded(std::string_view a, std::string_view b)
    {
        const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        {
            const int difference = lower(a[i]) - lower(b[i]);
            if (difference) { return difference; }
        }
        return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
    }
    void reset(bool enabled)
    {
        std::vector<Entry> retired;
        std::lock_guard guard(m_mutex);
        m_enabled = enabled; ++m_generation; retired.swap(m_entries); m_bytes = 0;
    }
    template <class Loader> Listing get(const std::string &path, Loader load)
    {
        uint64_t generation;
        {
            std::lock_guard guard(m_mutex);
            generation = m_generation;
            if (m_enabled)
                for (auto &entry : m_entries)
                    if (entry.path == path) { entry.used = ++m_clock; return entry.names; }
        }
        // Directory I/O is outside the cache lock, so unrelated worker reads do not wait on it.
        std::optional<Names> loaded = load();
        if (!loaded) { return {}; }
        std::sort(loaded->begin(), loaded->end(), [](const auto &a, const auto &b)
        { const int folded = compareFolded(a, b); return folded ? folded < 0 : a < b; });
        loaded->erase(std::unique(loaded->begin(), loaded->end()), loaded->end());
        size_t bytes = sizeof(Names) + loaded->capacity() * sizeof(std::string) + path.capacity();
        for (const auto &name : *loaded) { bytes += name.capacity() + 1; }
        auto names = std::make_shared<const Names>(std::move(*loaded));
        std::vector<Entry> retired;
        retired.reserve(8);
        std::lock_guard guard(m_mutex);
        if (!m_enabled || generation != m_generation || bytes > Budget) { return names; }
        for (auto &entry : m_entries)
            if (entry.path == path) { entry.used = ++m_clock; return entry.names; }
        while (!m_entries.empty() && (m_entries.size() >= 8 || m_bytes + bytes > Budget))
        {
            auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
                [](const auto &a, const auto &b) { return a.used < b.used; });
            m_bytes -= oldest->bytes; retired.push_back(std::move(*oldest)); m_entries.erase(oldest);
        }
        m_entries.push_back({path, names, bytes, ++m_clock}); m_bytes += bytes;
        return names;
    }
    static std::optional<std::string> find(const Listing &names, std::string_view requested)
    {
        if (!names) { return {}; }
        const auto it = std::lower_bound(names->begin(), names->end(), requested,
            [](const std::string &a, std::string_view b) { return compareFolded(a, b) < 0; });
        return it != names->end() && compareFolded(*it, requested) == 0 ? std::optional<std::string>(*it) : std::nullopt;
    }
private:
    static constexpr size_t Budget = 4 * 1024 * 1024;
    struct Entry { std::string path; Listing names; size_t bytes; uint64_t used; };
    std::mutex m_mutex;
    std::vector<Entry> m_entries;
    uint64_t m_generation = 0, m_clock = 0;
    size_t m_bytes = 0;
    bool m_enabled = true;
};
}
