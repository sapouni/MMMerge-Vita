#pragma once

#include <array>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OpenYAMM::Game
{
// Immutable readers share pages. An update copies only a touched page/shard;
// no reader ever sees a mutation to storage retained by another PathMap.
template<class T, size_t PageSize = 32>
class PathSnapshotVector
{
    using Page = std::vector<T>;
    std::vector<std::shared_ptr<Page>> m_pages;
    size_t m_size = 0;

public:
    PathSnapshotVector() = default;
    PathSnapshotVector(const PathSnapshotVector &) = default;
    PathSnapshotVector &operator=(const PathSnapshotVector &) = default;
    PathSnapshotVector(PathSnapshotVector &&other) noexcept
        : m_pages(std::move(other.m_pages)), m_size(std::exchange(other.m_size, 0)) {}
    PathSnapshotVector &operator=(PathSnapshotVector &&other) noexcept
    {
        if (this != &other) { m_pages = std::move(other.m_pages); m_size = std::exchange(other.m_size, 0); }
        return *this;
    }
    size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    const T &operator[](size_t index) const { return (*m_pages[index / PageSize])[index % PageSize]; }
    T &edit(size_t index)
    {
        std::shared_ptr<Page> &page = m_pages[index / PageSize];
        if (page.use_count() != 1) { page = std::make_shared<Page>(*page); }
        return (*page)[index % PageSize];
    }
    void clear() { m_pages.clear(); m_size = 0; }
    void reserve(size_t size) { m_pages.reserve((size + PageSize - 1) / PageSize); }
    void push_back(T value)
    {
        if (m_size % PageSize == 0)
        {
            auto page = std::make_shared<Page>();
            page->reserve(PageSize);
            page->push_back(std::move(value));
            m_pages.push_back(std::move(page));
            ++m_size;
            return;
        }
        std::shared_ptr<Page> &page = m_pages.back();
        if (page.use_count() != 1) { page = std::make_shared<Page>(*page); }
        page->push_back(std::move(value));
        ++m_size;
    }
    void emplace_back() { push_back(T{}); }
    void assign(std::vector<T> values)
    {
        clear();
        reserve(values.size());
        for (T &value : values) { push_back(std::move(value)); }
    }
};

template<class Key, class Value, class Hash = std::hash<Key>, size_t ShardCount = 256>
class PathSnapshotMap
{
    using Shard = std::unordered_map<Key, Value, Hash>;
    std::array<std::shared_ptr<Shard>, ShardCount> m_shards{};
    size_t m_size = 0;
    Shard &writable(const Key &key)
    {
        std::shared_ptr<Shard> &shard = m_shards[Hash{}(key) % ShardCount];
        if (!shard) { shard = std::make_shared<Shard>(); }
        else if (shard.use_count() != 1) { shard = std::make_shared<Shard>(*shard); }
        return *shard;
    }

public:
    void clear() { m_shards = {}; m_size = 0; }
    bool empty() const { return m_size == 0; }
    const Value *get(const Key &key) const
    {
        const std::shared_ptr<Shard> &shard = m_shards[Hash{}(key) % ShardCount];
        if (!shard) { return nullptr; }
        const auto found = shard->find(key);
        return found == shard->end() ? nullptr : &found->second;
    }
    Value &edit(const Key &key)
    {
        auto [found, inserted] = writable(key).try_emplace(key);
        m_size += inserted;
        return found->second;
    }
    void insertIfAbsent(const Key &key, Value value)
    {
        if (!get(key)) { edit(key) = std::move(value); }
    }
    void erase(const Key &key)
    {
        if (get(key)) { m_size -= writable(key).erase(key); }
    }
};
}
