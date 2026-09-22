// Reconstructible display cache. Never accepted as gameplay state or save validation.
namespace SaveMenuCache
{
constexpr size_t MaxPreview = 1024 * 1024, MaxText = 4096;
struct Identity
{
    uint64_t size = 0, modified = 0, edgeHash = 0;
    bool operator==(const Identity &) const = default;
};
inline uint64_t digest(const void *data, size_t count, uint64_t hash = 14695981039346656037ull)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}
inline std::filesystem::path cachePath(const std::filesystem::path &path)
{
    std::filesystem::path result = path;
    result += ".menu-cache";
    return result;
}
inline std::optional<Identity> identify(const std::filesystem::path &path)
{
    std::error_code error;
    Identity id;
    id.size = std::filesystem::file_size(path, error);
    if (error || id.size < 12) return std::nullopt;
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) return std::nullopt;
    id.modified = uint64_t(modified.time_since_epoch().count());
    std::ifstream input(path, std::ios::binary);
    std::array<char, 4096> edges;
    const size_t first = std::min<uint64_t>(id.size, edges.size());
    if (!input.read(edges.data(), first)) return std::nullopt;
    id.edgeHash = digest(edges.data(), first);
    if (id.size > first)
    {
        const size_t last = std::min<uint64_t>(id.size - first, edges.size());
        input.seekg(id.size - last);
        if (!input.read(edges.data(), last)) return std::nullopt;
        id.edgeHash = digest(edges.data(), last, id.edgeHash);
    }
    return id;
}
struct Header
{
    uint32_t magic = 0x324d594f, version = 1;
    Identity identity;
    uint32_t mapBytes = 0, nameBytes = 0, previewBytes = 0;
    float minutes = 0;
    uint64_t checksum = 0;
};
static_assert(sizeof(Header) == 56);
template <typename Summary>
inline uint64_t checksum(Header header, const Summary &summary)
{
    header.checksum = 0;
    uint64_t hash = digest(&header, sizeof(header));
    hash = digest(summary.mapFileName.data(), summary.mapFileName.size(), hash);
    hash = digest(summary.saveName.data(), summary.saveName.size(), hash);
    return digest(summary.previewBmp.data(), summary.previewBmp.size(), hash);
}
inline std::optional<SaveMenuSummary> read(const std::filesystem::path &path, const Identity &id, bool includePreview = true)
{
    std::ifstream input(cachePath(path), std::ios::binary);
    Header header;
    if (!input.read(reinterpret_cast<char *>(&header), sizeof(header))
        || header.magic != 0x324d594f || header.version != 1 || !(header.identity == id)
        || header.mapBytes > MaxText || header.nameBytes > MaxText || header.previewBytes > MaxPreview)
        return std::nullopt;
    SaveMenuSummary result;
    result.savedGameMinutes = header.minutes;
    result.mapFileName.resize(header.mapBytes); result.saveName.resize(header.nameBytes);
    if (!input.read(result.mapFileName.data(), header.mapBytes)
        || !input.read(result.saveName.data(), header.nameBytes)) return std::nullopt;
    Header hashHeader = header; hashHeader.checksum = 0;
    uint64_t hash = digest(&hashHeader, sizeof(hashHeader));
    hash = digest(result.mapFileName.data(), result.mapFileName.size(), hash);
    hash = digest(result.saveName.data(), result.saveName.size(), hash);
    if (includePreview)
    {
        result.previewBmp.resize(header.previewBytes);
        if (!input.read(reinterpret_cast<char *>(result.previewBmp.data()), header.previewBytes)) return std::nullopt;
        hash = digest(result.previewBmp.data(), result.previewBmp.size(), hash);
    }
    else
    {
        std::array<char, 4096> chunk;
        size_t remaining = header.previewBytes;
        while (remaining)
        {
            const size_t count = std::min(remaining, chunk.size());
            if (!input.read(chunk.data(), count)) return std::nullopt;
            hash = digest(chunk.data(), count, hash); remaining -= count;
        }
    }
    if (input.peek() != std::char_traits<char>::eof() || hash != header.checksum) return std::nullopt;
    return result;
}
template <typename Summary>
inline void write(const std::filesystem::path &path, const Identity &id, const Summary &summary) noexcept
{
    try
    {
        if (summary.mapFileName.size() > MaxText || summary.saveName.size() > MaxText
            || summary.previewBmp.size() > MaxPreview) return;
        Header header;
        header.identity = id; header.mapBytes = uint32_t(summary.mapFileName.size());
        header.nameBytes = uint32_t(summary.saveName.size()); header.previewBytes = uint32_t(summary.previewBmp.size());
        header.minutes = summary.savedGameMinutes; header.checksum = checksum(header, summary);
        // A torn display cache is rejected next time. No write touches the actual save.
        std::ofstream output(cachePath(path), std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(&header), sizeof(header));
        output.write(summary.mapFileName.data(), header.mapBytes);
        output.write(summary.saveName.data(), header.nameBytes);
        output.write(reinterpret_cast<const char *>(summary.previewBmp.data()), header.previewBytes);
    }
    catch (...) {} // Cache failure must not turn a valid save into a load failure.
}
inline void invalidate(const std::filesystem::path &path) noexcept
{
    try { std::error_code ignored; std::filesystem::remove(cachePath(path), ignored); }
    catch (...) {}
}
}
