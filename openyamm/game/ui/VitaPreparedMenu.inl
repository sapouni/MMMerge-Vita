// Lossless prepared menu rows. Assets are optional and source/content validated.
// No full PNG source, decoded image, or second full upload allocation on this path.
#if defined(OPENYAMM_VITA_LOADER)
uint32_t vitaMenuHash(uint32_t hash, const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}
bool vitaMenuReadExact(Engine::AssetReadStream &stream, void *destination, size_t size)
{
    auto *out = static_cast<uint8_t *>(destination);
    while (size)
    {
        const int64_t n = stream.read(out, size);
        if (n <= 0 || uint64_t(n) > size) return false;
        out += n; size -= size_t(n);
    }
    return true;
}
std::optional<bgfx::TextureHandle> uploadPreparedMenuRows(
    const Engine::AssetFileSystem &assets, const std::string &sourcePath, int &width, int &height)
{
    const size_t slash = sourcePath.find_last_of("/\\");
    const std::string name = toLowerCopy(sourcePath.substr(slash == std::string::npos ? 0 : slash + 1));
    auto rows = assets.openReadStream("vita-menu-cache/" + name + ".oyui");
    if (!rows) return std::nullopt;
    uint32_t h[8]{};
    if (!vitaMenuReadExact(*rows, h, sizeof(h)) || h[0] != 0x4955594fu || h[1] != 1
        || !h[2] || !h[3] || h[2] > 4096 || h[3] > 4096 || h[7] != 0
        || rows->length() != int64_t(sizeof(h) + uint64_t(h[2])*h[3]*4)) return std::nullopt;
    auto source = assets.openReadStream(sourcePath);
    if (!source || source->length() != h[4]) return std::nullopt;
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    try
    {
        std::vector<uint8_t> buffer(65536);
        uint32_t sourceHash = 2166136261u;
        for (uint64_t left = h[4]; left;)
        {
            const size_t count = size_t(std::min<uint64_t>(left, buffer.size()));
            if (!vitaMenuReadExact(*source, buffer.data(), count)) return std::nullopt;
            sourceHash = vitaMenuHash(sourceHash, buffer.data(), count); left -= count;
        }
        if (sourceHash != h[5]) return std::nullopt; // Different world/updated source: normal decoder.
        source.reset();
        width = int(h[2]); height = int(h[3]);
        texture = createEmptyBgraTexture2D(uint16_t(width),uint16_t(height),TextureFilterProfile::Ui,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (!bgfx::isValid(texture)) return texture;
        const unsigned rowBytes = unsigned(width)*4, rowsPerChunk = unsigned(buffer.size())/rowBytes;
        uint32_t payloadHash = 2166136261u;
        for (unsigned row = 0; row < unsigned(height);)
        {
            const unsigned count = std::min(rowsPerChunk, unsigned(height)-row);
            if (!vitaMenuReadExact(*rows,buffer.data(),count*rowBytes))
            { bgfx::destroy(texture); return std::nullopt; }
            payloadHash = vitaMenuHash(payloadHash,buffer.data(),count*rowBytes);
            const bgfx::Memory *memory = copyBgraTextureUploadMemory(buffer.data(),count*rowBytes);
            if (!memory) { bgfx::destroy(texture); return bgfx::TextureHandle{bgfx::kInvalidHandle}; }
            bgfx::updateTexture2D(texture,0,0,0,uint16_t(row),uint16_t(width),uint16_t(count),memory);
            row += count;
        }
        if (payloadHash != h[6]) { bgfx::destroy(texture); return std::nullopt; }
        std::cerr << "VITA prepared menu rows: " << sourcePath << " " << width << 'x' << height << '\n';
        return texture;
    }
    catch (const std::bad_alloc &)
    {
        if (bgfx::isValid(texture)) bgfx::destroy(texture);
        // Matched cache allocation pressure must not fall into the larger PNG allocation.
        return bgfx::TextureHandle{bgfx::kInvalidHandle};
    }
}
#endif
