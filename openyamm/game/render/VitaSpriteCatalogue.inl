// Worker-owned address book, validated against sprite/palette ZIP content metadata.
#include "VitaSpriteArchiveIdentity.inl"
struct SpriteCatalogue
{
    struct Record
    {
        char name[128]{}, key[64]{};
        uint32_t palette = 0, width = 0, height = 0, bytes = 0;
    };
    static_assert(sizeof(Record) == 208);
    std::unordered_map<std::string, Record> records;
    std::string file;
    bool valid = true, dirty = false;
    static std::string identity(const std::string &name, int16_t palette)
    { return name + ':' + std::to_string(palette); }
    SpriteCatalogue(const Engine::AssetFileSystem &files, const std::string &world)
    {
        std::string revision = "sprite-catalogue-85:" + world + ':'
            + std::to_string(int(files.getAssetScaleTierForVirtualPath("Data/sprites")));
        const auto mounts=files.getSearchPaths();
        if(mounts.empty()) return;
        for(const auto &mount:mounts) if(!spriteArchiveIdentity(mount,revision)) return;
        file = cachePath(hashBytes(reinterpret_cast<const uint8_t *>(revision.data()), revision.size()));
        file.replace(file.size() - 5, 5, ".index");
        int fd = ::open(file.c_str(), O_RDONLY, 0);
        if(fd<0) {
            const auto slash=file.find_last_of('/');
            const std::string prebuilt="/app/runtime/sprite-catalogues/"+file.substr(slash+1);
            fd=::open(prebuilt.c_str(),O_RDONLY,0);
        }
        if (fd < 0) return;
        struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
        const auto bytes = ::lseek(fd, 0, SEEK_END);
        if (bytes < 0 || bytes > int64_t(16384 * (sizeof(Record) + 32))
            || bytes % (sizeof(Record) + 32) || ::lseek(fd, 0, SEEK_SET) < 0) { valid = false; return; }
        auto readAll = [&](void *destination, size_t count)
        {
            auto *p = static_cast<uint8_t *>(destination);
            while (count)
            {
                const auto got = ::read(fd, p, count);
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) return false;
                p += got; count -= size_t(got);
            }
            return true;
        };
        // Read 68 checksummed records per transfer instead of two small reads per record.
        std::array<uint8_t, 16320> block{};
        for (int64_t offset = 0; offset < bytes;)
        {
            const size_t count = size_t(std::min<int64_t>(block.size(), bytes - offset));
            if (!readAll(block.data(), count)) { records.clear(); valid = false; return; }
            for (size_t i = 0; i < count; i += sizeof(Record) + 32)
            {
                Record record; std::array<uint8_t,32> digest;
                std::memcpy(&record, block.data() + i, sizeof(record));
                std::memcpy(digest.data(), block.data() + i + sizeof(record), digest.size());
                if (digest != hashBytes(reinterpret_cast<const uint8_t *>(&record), sizeof(record))
                    || !std::memchr(record.name, 0, sizeof(record.name))
                    || !SpriteContainer::validKey(record.key) || !record.width || !record.height
                    || record.width > 4096 || record.height > 4096 || record.bytes > ReadyLimit)
                { records.clear(); valid = false; return; }
                records[identity(record.name, int16_t(record.palette))] = record;
            }
            offset += count;
        }
    }
    ~SpriteCatalogue() { try { flush(); } catch (...) {} }
    void flush()
    {
        if (!dirty || file.empty()) return;
        // Commit on orderly worker/map shutdown, not each temporary queue gap during play.
        // A torn cache is rejected by record length/checksum on the next launch.
        const std::string temporary = file + ".tmp";
        const int output = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (output < 0) return;
        std::array<uint8_t, 16320> buffer{}; // 68 complete checksummed records.
        size_t used = 0;
        const auto drain = [&]()
        {
            size_t sent = 0;
            while (sent < used)
            {
                const auto n = ::write(output, buffer.data() + sent, used - sent);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return false;
                sent += size_t(n);
            }
            used = 0; return true;
        };
        bool written = true;
        for (const auto &[id,record] : records)
        {
            try
            {
                const auto digest = hashBytes(reinterpret_cast<const uint8_t *>(&record), sizeof(record));
                std::memcpy(buffer.data()+used, &record, sizeof(record)); used += sizeof(record);
                std::memcpy(buffer.data()+used, digest.data(), digest.size()); used += digest.size();
                if (used == buffer.size() && !drain()) { written = false; break; }
            }
            catch (...) { written = false; break; }
        }
        if (written && used) written = drain();
        const bool closed = ::close(output) == 0;
        if (written && closed && std::rename(temporary.c_str(), file.c_str()) != 0)
        {
            // Some targets refuse rename-over-existing. Keep the old cache until
            // the complete replacement is closed; restore it if the second rename fails.
            const std::string backup = file + ".previous";
            std::remove(backup.c_str());
            written = std::rename(file.c_str(), backup.c_str()) == 0;
            if (written)
            {
                written = std::rename(temporary.c_str(), file.c_str()) == 0;
                if (written) std::remove(backup.c_str());
                else std::rename(backup.c_str(), file.c_str());
            }
        }
        if (written && closed) { dirty = false; valid = true; }
    }
    const Record *find(const std::string &name, int16_t palette) const
    {
        const auto found = records.find(identity(name, palette));
        return found == records.end() ? nullptr : &found->second;
    }
    void remember(const std::string &name, int16_t palette, const std::string &path,
        const VitaSpriteImage &image)
    {
        if (file.empty() || name.size() >= sizeof(Record::name) || records.size() >= 16384) return;
        const std::string id = identity(name, palette);
        if (records.contains(id)) return;
        Record record;
        std::memcpy(record.name, name.data(), name.size());
        const std::string key = SpriteContainer::keyFor(path);
        if (key.size() != 64 || !SpriteContainer::validKey(key.data())) return;
        std::memcpy(record.key, key.data(), 64);
        record.palette = uint16_t(palette);
        record.width = image.sourceWidth; record.height = image.sourceHeight;
        record.bytes = uint32_t(image.pixels.size() + image.maskBits.size());
        records.emplace(id, record);
        dirty = true;
    }
};
