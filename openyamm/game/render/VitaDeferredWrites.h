#pragma once
#include <array>
#include <cstddef>
#include <memory>

namespace OpenYAMM::Game
{
// Worker-owned. Keeps immutable deliveries alive without a copy or a second I/O thread.
// Full queues skip optional persistence, never image delivery.
template<class Key, class Image, size_t Count = 32, size_t Limit = 2 * 1024 * 1024>
class VitaDeferredWrites
{
public:
    struct Item { Key key{}; std::shared_ptr<Image> image; size_t bytes = 0; };
    bool push(Key key, const std::shared_ptr<Image> &image, size_t bytes)
    {
        for (const auto &item : m_items) if (item.image && item.key == key) return true;
        if (m_count == Count || bytes > Limit - m_bytes) { ++skipped; return false; }
        for (auto &item : m_items) if (!item.image)
        {
            item = {key, image, bytes}; ++m_count; m_bytes += bytes;
            if (m_bytes > peakBytes) peakBytes = m_bytes;
            return true;
        }
        return false;
    }
    std::shared_ptr<Image> find(Key key) const
    {
        for (const auto &item : m_items) if (item.image && item.key == key) return item.image;
        return {};
    }
    Item pop()
    {
        for (auto &item : m_items) if (item.image)
        {
            Item result = std::move(item); item = {}; --m_count; m_bytes -= result.bytes;
            return result;
        }
        return {};
    }
    bool empty() const { return m_count == 0; }
    size_t size() const { return m_count; }
    size_t bytes() const { return m_bytes; }
    size_t skipped = 0, peakBytes = 0;
private:
    std::array<Item, Count> m_items{};
    size_t m_count = 0, m_bytes = 0;
};
}
