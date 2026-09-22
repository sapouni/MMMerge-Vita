// Included inside VitaSpriteStream's private namespace. All access is on the sprite worker.
struct SpriteContainer
{
    struct Record { char key[64]; uint32_t offset, bytes, format; };
    struct Manifest
    {
        uint32_t magic, count, packBytes, reserved;
        std::array<uint8_t, 32> checksum;
    };
    static_assert(sizeof(Record) == 76 && sizeof(Manifest) == 48);
    std::unordered_map<std::string, Record> packed, journal;
    std::array<int,3> descriptors{-1,-1,-1}; // full, reduced, append journal
    ~SpriteContainer() { for (int fd : descriptors) if (fd >= 0) ::close(fd); }
    static bool readAll(int fd, void *destination, size_t bytes)
    {
        auto *out = static_cast<uint8_t *>(destination);
        while (bytes)
        {
            const auto count = ::read(fd, out, bytes);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            out += count; bytes -= size_t(count);
        }
        return true;
    }
    bool loaded = false, journalValid = true;
    uint32_t journalBytes = 0;
    static constexpr uint32_t MaxPackBytes = 256 * 1024 * 1024;
    static std::string keyFor(const std::string &path) { return path.substr(path.find_last_of('/') + 1, 64); }
    static bool validKey(const char *key)
    {
        for (unsigned i = 0; i < 64; ++i)
        {
            if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f'))) return false;
        }
        return true;
    }
    static std::string filePath(const char *name) { return std::string(CacheRoot) + '/' + name; }
    // Raw read handles use the loader's native asset path, including close.
    // stdio used to re-enter newlib's global descriptor lock on every manifest close.
    struct File
    {
        int fd = -1;
        File(int value = -1) : fd(value) {}
        ~File() { if (fd >= 0) ::close(fd); }
        File(const File &) = delete;
        File &operator=(const File &) = delete;
        explicit operator bool() const { return fd >= 0; }
        int get() const { return fd; }
        void reset(int value) { if (fd >= 0) ::close(fd); fd = value; }
    };
    static int openRead(const char *name) { return ::open(filePath(name).c_str(), O_RDONLY, 0); }
    static bool atEnd(int fd) { uint8_t byte; return ::read(fd, &byte, 1) == 0; }
    static int64_t fileBytes(int file)
    {
        const int64_t bytes = ::lseek(file, 0, SEEK_END);
        return ::lseek(file, 0, SEEK_SET) < 0 ? -1 : bytes;
    }
    void load()
    {
        if (loaded) return;
        loaded = true;
        File data;
        for (bool low : {false, true})
        {
            if (low && !vitaPerformanceSettings().spriteHalfResolution) continue;
            File index(openRead(low ? "sprites-low.manifest" : "sprites.manifest"));
            data.reset(openRead(low ? "sprites-low.pack" : "sprites.pack"));
            Manifest header{};
            if (!index || !data || !readAll(index.get(), &header, sizeof(header))
                || header.magic != 0x384d594f || header.reserved || header.count > 16384
                || header.packBytes > MaxPackBytes || fileBytes(data.get()) != header.packBytes) continue;
            std::vector<Record> records(header.count);
            const size_t bytes = records.size() * sizeof(Record);
            bool valid = readAll(index.get(), records.data(), bytes)
                && atEnd(index.get())
                && hashBytes(reinterpret_cast<const uint8_t *>(records.data()), bytes) == header.checksum;
            for (const Record &record : records)
                valid = valid && validKey(record.key) && record.bytes >= sizeof(Header)
                    && record.bytes <= ReadyLimit + sizeof(Header)
                    && uint64_t(record.offset) + record.bytes <= header.packBytes
                    && (low ? record.format == 0x3653594f
                        : (record.format == 0x3553594f || record.format == 0x3153594f));
            if (valid) for (const Record &record : records) packed[std::string(record.key, 64)] = record;
        }
        // Runtime records carry their own index and payload checksum. A torn tail is discarded on rebuild.
        data.reset(openRead("runtime.pack"));
        if (data)
        {
            const long total = fileBytes(data.get());
            journalValid = total >= 0 && total <= MaxPackBytes;
            File runtimeIndex(openRead("runtime.manifest"));
            Manifest header{};
            if (journalValid && runtimeIndex && readAll(runtimeIndex.get(), &header, sizeof(header))
                && header.magic==0x384d594f && !header.reserved && header.count<=16384 && header.packBytes==uint32_t(total))
            {
                std::vector<Record> records(header.count);
                const size_t bytes=records.size()*sizeof(Record);
                bool valid=readAll(runtimeIndex.get(), records.data(), bytes)
                    && atEnd(runtimeIndex.get())
                    && hashBytes(reinterpret_cast<const uint8_t *>(records.data()),bytes)==header.checksum;
                uint32_t end=0;
                for (const auto &record : records)
                {
                    valid=valid && validKey(record.key) && record.offset==end+sizeof(Record)
                        && record.bytes>=sizeof(Header) && record.bytes<=ReadyLimit+sizeof(Header)
                        && uint64_t(record.offset)+record.bytes<=uint32_t(total)
                        && (record.format==0x3153594f || record.format==0x3553594f);
                    end=record.offset+record.bytes;
                }
                if (valid && end==uint32_t(total))
                {
                    for (const auto &record : records) journal[std::string(record.key,64)]=record;
                    journalBytes=end;
                }
            }
            while (journalValid && journalBytes < uint32_t(total))
            {
                Record record{};
                journalValid = readAll(data.get(), &record, sizeof(record))
                    && validKey(record.key) && record.offset == journalBytes + sizeof(record)
                    && record.bytes >= sizeof(Header) && record.bytes <= ReadyLimit + sizeof(Header)
                    && uint64_t(record.offset) + record.bytes <= uint32_t(total)
                    && (record.format == 0x3153594f || record.format == 0x3553594f);
                if (!journalValid) break;
                journal[std::string(record.key, 64)] = record;
                journalBytes = record.offset + record.bytes;
                journalValid = ::lseek(data.get(), journalBytes, SEEK_SET) == journalBytes;
            }
            if (!journalValid) journal.clear();
        }
        std::fprintf(stderr, "VITA SPRITE_CONTAINER packed=%u journal=%u valid_journal=%u rebuild_on_miss=1\n",
            unsigned(packed.size()), unsigned(journal.size()), unsigned(journalValid));
    }
    size_t preparedBytes(const std::string &path)
    {
        load();
        const std::string key = keyFor(path);
        for (const auto *index : {&packed, &journal})
        {
            const auto found = index->find(key);
            if (found != index->end()) return found->second.bytes - sizeof(Header);
        }
        return 0;
    }
    std::optional<VitaSpriteImage> readRecord(const Record &record, const char *name, bool allowBc3, const std::shared_ptr<Engine::BufferPool<uint8_t>> &pool)
    {
        const bool reduced = record.format == 0x3653594f;
        const bool bc3 = reduced || record.format == 0x3553594f;
        if (bc3 && !allowBc3) return {};
        const unsigned slot = std::strcmp(name, "runtime.pack") == 0 ? 2 : (reduced ? 1 : 0);
        int &fd = descriptors[slot];
        if (fd < 0) fd = ::open(filePath(slot == 2 ? "runtime.pack" : (reduced ? "sprites-low.pack" : "sprites.pack")).c_str(), O_RDONLY, 0);
        if (fd < 0 || ::lseek(fd, record.offset, SEEK_SET) < 0) return {};
        Header header;
        if (!readAll(fd, &header, sizeof(header)) || header.magic != record.format || !header.width || !header.height
            || header.width > 4096 || header.height > 4096) return {};
        const uint32_t sourceWidth = header.width, sourceHeight = header.height;
        if (reduced) { header.width = (header.width+1)/2; header.height = (header.height+1)/2; }
        const size_t textureBytes = bc3 ? size_t((header.width + 3) / 4) * ((header.height + 3) / 4) * 16
            : size_t(header.width) * header.height * 4;
        const size_t maskBytes = bc3 ? (size_t(sourceWidth) * sourceHeight + 7) / 8 : 0;
        if (header.bytes != textureBytes + maskBytes || header.bytes > ReadyLimit
            || record.bytes != sizeof(header) + header.bytes) return {};
        VitaSpriteImage image{int(header.width), int(header.height), pool->take(header.bytes)};
        image.recycler = pool;
        image.sourceWidth = sourceWidth; image.sourceHeight = sourceHeight;
        if (!readAll(fd, image.pixels.data(), header.bytes)
            || hashBytes(image.pixels.data(), image.pixels.size()) != header.checksum) return {};
        image.bc3 = bc3;
        if (bc3)
        {
            image.maskBits = pool->take(maskBytes);
            std::copy(image.pixels.begin() + textureBytes, image.pixels.end(), image.maskBits.begin());
            image.pixels.resize(textureBytes);
        }
        return image;
    }
    std::optional<VitaSpriteImage> read(const std::string &path, bool allowBc3, const std::shared_ptr<Engine::BufferPool<uint8_t>> &pool)
    {
        load();
        const std::string key = keyFor(path);
        // Prefer validated offline BC3 to an older BGRA runtime copy; retain repair fallback.
        for (bool runtime : {false, true})
        {
            auto &records = runtime ? journal : packed;
            const auto found = records.find(key);
            if (found == records.end()) continue;
            auto image = readRecord(found->second, runtime ? "runtime.pack" : "sprites.pack", allowBc3, pool);
            if (image) return image;
            records.erase(found);
        }
        return {};
    }
    bool write(const std::string &path, const VitaSpriteImage &image)
    {
        load();
        const std::string key = keyFor(path);
        if (key.size() != 64 || !validKey(key.data()) || image.pixels.size() > ReadyLimit) return false;
        Record record{};
        std::memcpy(record.key, key.data(), 64);
        record.bytes = uint32_t(sizeof(Header) + image.pixels.size() + image.maskBits.size());
        record.format = image.bc3 ? 0x3553594f : 0x3153594f;
        if (uint64_t(journalBytes) + sizeof(record) + record.bytes > MaxPackBytes) journalValid = false;
        // No cached read buffer may survive append/truncation of this file.
        if (descriptors[2] >= 0) { ::close(descriptors[2]); descriptors[2] = -1; }
        const int file = ::open(filePath("runtime.pack").c_str(), O_WRONLY | O_CREAT
            | (journalValid ? O_APPEND : O_TRUNC), 0666);
        if (file < 0) return false;
        if (!journalValid) { journal.clear(); journalBytes = 0; journalValid = true; }
        record.offset = journalBytes + sizeof(record);
        Header header;
        header.magic = record.format;
        header.width = image.width; header.height = image.height; header.bytes = uint32_t(image.pixels.size()+image.maskBits.size());
        header.checksum = hashBytes(image.pixels.data(), image.pixels.size(), nullptr, image.maskBits.data(), image.maskBits.size());
        const auto writeAll = [&](const void *source, size_t size)
        {
            const auto *bytes = static_cast<const uint8_t *>(source);
            while (size)
            {
                const auto n = ::write(file, bytes, std::min<size_t>(size, 16384));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return false;
                bytes += n; size -= size_t(n);
            }
            return true;
        };
        bool ok = writeAll(&record, sizeof(record)) && writeAll(&header, sizeof(header))
            && writeAll(image.pixels.data(), image.pixels.size())
            && writeAll(image.maskBits.data(), image.maskBits.size());
        ok = ::close(file) == 0 && ok;
        if (!ok) { journalValid = false; return false; }
        journalBytes = record.offset + record.bytes;
        journal[key] = record;
        return true;
    }
};
thread_local SpriteContainer spriteContainer;
