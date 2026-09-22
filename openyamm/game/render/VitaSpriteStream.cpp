#include "engine/VitaStall.h"
#include "engine/VitaInitialView.h"
#include "engine/VitaLoadProfile.h"
#include "engine/VitaVisibilityProfile.h"
#include "game/render/VitaAnimationBudget.h"
#include "VitaSpriteStream.h"
#if defined(OPENYAMM_VITA_LOADER)
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_hints.h>
#include "TextureFiltering.h"
#include "VitaPerformanceConfig.h"
#include "VitaSpriteHeader.h"
#include "VitaDeferredWrites.h"
#include "game/ui/GameplayHudCommon.h"
#include "engine/VitaFrameProfile.h"
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
extern "C"
{
#include <libavutil/sha.h>
#include <libavutil/mem.h>
}

#define STB_DXT_STATIC
#define STB_DXT_IMPLEMENTATION
#include "VitaStbDxt.inl"

namespace OpenYAMM::Game
{
namespace
{
constexpr size_t ReadyLimit = 4 * 1024 * 1024;
constexpr size_t QueueLimit = 32;
constexpr size_t ReadyQueueLimit = 1024 * 1024;
constexpr const char *CacheRoot = "/data/sprite-cache-v3";
std::array<uint8_t, 32> hashBytes(const uint8_t *bytes, size_t size,
    const Engine::ImageDecodeOptions *options = nullptr,
    const uint8_t *tail = nullptr, size_t tailSize = 0)
{
    std::array<uint8_t, 32> hash{};
    static thread_local std::unique_ptr<AVSHA, decltype(&av_free)> stored(av_sha_alloc(), av_free);
    if (!stored) stored.reset(av_sha_alloc());
    AVSHA *context = stored.get();
    if (!context) { throw std::bad_alloc(); }
    av_sha_init(context, 256);
    av_sha_update(context, bytes, size);
    if (options)
    {
        // Version includes the exact palette-zero, BGRA and edge-bleed algorithm, with no mipmaps.
        const uint8_t version[] = {1, uint8_t(options->overridePalette.has_value())};
        av_sha_update(context, version, sizeof(version));
        if (options->overridePalette)
        {
            av_sha_update(context, options->overridePalette->data(), options->overridePalette->size());
        }
    }
    if (tailSize) av_sha_update(context, tail, tailSize);
    av_sha_final(context, hash.data());
    return hash;
}
std::string cachePath(const std::array<uint8_t, 32> &hash)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string path = std::string(CacheRoot) + '/';
    for (uint8_t byte : hash) { path += hex[byte >> 4]; path += hex[byte & 15]; }
    return path + ".bgra";
}
// Explicit fixed-width header, validated before allocation. Cache is local little-endian data.
struct Header
{
    uint32_t magic = 0x3153594f, width = 0, height = 0, bytes = 0;
    std::array<uint8_t, 32> checksum{};
};
static_assert(sizeof(Header) == 48);
#include "VitaSpriteContainer.inl"
std::optional<VitaSpriteImage> readDerived(const std::string &path, bool allowBc3, const std::shared_ptr<Engine::BufferPool<uint8_t>> &pool)
{
    OY_LOAD(Engine::VitaLoad::Scope read(Engine::VitaLoad::DerivedRead));
    auto image = spriteContainer.read(path, allowBc3, pool);
    OY_LOAD(Engine::VitaLoad::derived(image.has_value(), image && image->bc3));
    OY_LOAD(if (image) read.bytes(image->pixels.size() + image->maskBits.size()));
    return image;
}
bool writeDerived(const std::string &path, const VitaSpriteImage &image)
{
    OY_LOAD(Engine::VitaLoad::Scope write(Engine::VitaLoad::DerivedWrite));
    return spriteContainer.write(path, image);
}

#include "VitaSpriteCatalogue.inl"
#include "VitaSpriteCompression.inl"
}

struct VitaSpriteStream::State
{
    struct Entry
    {
        std::string name, path;
        int16_t palette = 0;
        uint64_t touched = 0, backing = 0, payloadBytes = 0, reservationBytes = 0;
        bool pending = false, failed = false;
        uint32_t ticket = 0;
        Entry *nextReady = nullptr;
        uint64_t readyObservedFrame = 0;
        bool offered = false;
        uint64_t retryAfter = 0;
        unsigned attempts = 0, failureReason = 0;
        std::shared_ptr<VitaSpriteImage> ready;
    };
    const Engine::AssetFileSystem &files;
    std::string world;
    GameplayAssetLoadCache cache; // Worker-only. Never shares mutable view caches.
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Entry> entries;
    std::deque<Entry *> queue;
    Entry *readyHead = nullptr;
    bool stop = false;
    const bool allowBc3 = bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::BC3, 0);
    size_t readyBytes = 0, readyPeak = 0;
    uint64_t bc3Hits = 0, catalogueHits = 0, retries = 0;
    uint64_t hits = 0, decoded = 0, failures = 0, writesFailed = 0;
    uint64_t readUs = 0, decodeUs = 0, bleedUs = 0, writeUs = 0, queueFull = 0;
    uint64_t consumed = 0, expired = 0;
    uint64_t setupUs = 0, readyWaitUs = 0, preparedUs = 0;
    std::shared_ptr<Engine::BufferPool<uint8_t>> pool = std::make_shared<Engine::BufferPool<uint8_t>>(1024*1024);
    VitaDeferredWrites<Entry *, VitaSpriteImage> persistence;
    std::thread worker;
    State(const Engine::AssetFileSystem &source, std::string id) : files(source), world(std::move(id))
    {
        // Mount metadata is prepared on the loading path, before this worker can need it.
        // Empty filename warms only the directory; it loads no sprite pixels.
        files.findDirectoryEntry("Data/sprites", "");
        files.findDirectoryEntry("Data/icons", "");
        worker = std::thread([this] { run(); });
    }
    ~State()
    {
        { std::lock_guard lock(mutex); stop = true; }
        wake.notify_all();
        worker.join();
    }
    void run()
    {
        SDL_SetHint("openyamm_vita_thread_role", "asset");
        std::fprintf(stderr, "VITA SPRITES BC3_upload=%u\n", unsigned(allowBc3));
        mkdir(CacheRoot, 0777);
        Engine::VitaStall::workerStage(1);
        std::optional<SpriteCatalogue> catalogue;
        try { catalogue.emplace(files, world); } catch (...) {}
        for (;;)
        {
            bool idle;
            { std::lock_guard lock(mutex); idle = queue.empty(); }
            if (idle && !persistence.empty())
            {
                // Re-check demand between records. Never start a cache write with reads queued.
                auto pending = persistence.pop();
                Engine::VitaStall::workerStage(9, pending.key->ticket);
                const uint64_t begin = SDL_GetTicksNS();
                bool written = false;
                try { written = writeDerived(pending.key->path, *pending.image); } catch (...) {}
                Engine::VitaStall::activity(written ? Engine::VitaStall::CacheWriteDone : Engine::VitaStall::CacheWriteFailed);
                { std::lock_guard lock(mutex);
                    writeUs += (SDL_GetTicksNS() - begin) / 1000; writesFailed += !written; }
                continue;
            }
            // Catalogue destruction commits once at map/worker shutdown. A queue gap is not
            // permission to rewrite every known record while gameplay still needs the card.
            Engine::VitaStall::workerStage(0);
            Entry *entry;
            {
                std::unique_lock lock(mutex);
                // Unknown frames reserve the entire ready budget until their dimensions are known.
                wake.wait(lock, [&]
                {
                    if (stop) { return true; }
                    if (queue.empty()) { return false; }
                    return readyBytes < ReadyQueueLimit;
                });
                if (stop) { return; }
                entry = queue.front(); queue.pop_front();
            }
            std::optional<VitaSpriteImage> image;
            uint64_t localRead = 0, localDecode = 0, localBleed = 0, localWrite = 0;
            bool hit = false, writeFailed = false, permanent = false, catalogueHit = false, upgraded = false;
            unsigned reason = 0;
            int entryWidth = 0, entryHeight = 0;
            std::shared_ptr<VitaSpriteImage> delivery;
            uint64_t localSetup = 0, localWait = 0, localPrepared = 0;
            try
            {
                Engine::VitaStall::workerStage(2, entry->ticket);
                uint64_t begin = SDL_GetTicksNS();
                if (entry->path.empty() && catalogue)
                {
                    if (const auto *known = catalogue->find(entry->name, entry->palette))
                    {
                        entry->path = std::string(CacheRoot) + '/' + std::string(known->key, 64) + ".bgra";
                        entry->reservationBytes = known->bytes;
                        catalogueHit = true;
                    }
                }
                uint64_t reservation = entry->payloadBytes ? entry->payloadBytes : entry->reservationBytes;
                if (!entry->path.empty())
                {
                    const size_t preparedSize = spriteContainer.preparedBytes(entry->path);
                    if (preparedSize) reservation = preparedSize;
                }
                if (!reservation)
                {
                    int width = 0, height = 0;
                    if (!Engine::findImageAssetPath(files, "Data/sprites", entry->name,
                        cache.directoryAssetPathsByPath, cache.assetPathByKey))
                    { reason = 2; permanent = true; throw std::bad_alloc(); }
                    if (!readVitaSpriteDimensions(&files, cache, entry->name, width, height))
                    { reason = 3; throw std::bad_alloc(); }
                    if (uint64_t(width) * height * 4 > ReadyLimit)
                    { reason = 4; permanent = true; throw std::bad_alloc(); }
                    reservation = uint64_t(width) * height * 4;
                    entry->reservationBytes = reservation;
                }
                localSetup = (SDL_GetTicksNS() - begin) / 1000;
                Engine::VitaStall::workerStage(3, entry->ticket);
                const uint64_t waitBegin = SDL_GetTicksNS();
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [&] { return stop || readyBytes == 0 || readyBytes + reservation <= ReadyQueueLimit; });
                    if (stop) { return; }
                }
                localWait = (SDL_GetTicksNS() - waitBegin) / 1000;
                const auto preparedRead = [&]()
                {
                    Engine::VitaStall::workerStage(4, entry->ticket);
                    const uint64_t started = SDL_GetTicksNS();
                    auto result = readDerived(entry->path, allowBc3, pool);
                    localPrepared += (SDL_GetTicksNS() - started) / 1000;
                    return result;
                };
                // On GPU eviction this path reads only the prepared pixels, with no source lookup/decode.
                delivery = persistence.find(entry);
                if (delivery) { hit = true; }
                if (!delivery && !entry->path.empty()) { image = preparedRead(); }
                if (!delivery && !image)
                {
                    Engine::VitaStall::workerStage(5, entry->ticket);
                    const auto path = Engine::findImageAssetPath(files, "Data/sprites", entry->name,
                        cache.directoryAssetPathsByPath, cache.assetPathByKey);
                    Engine::ImageDecodeOptions options;
                    options.overridePalette = GameplayHudCommon::loadCachedActPalette(&files, cache, entry->palette, world);
                    options.applyPaletteZeroTransparencyKey = true;
                    const auto bytes = path ? files.readBinaryFile(*path) : std::nullopt;
                    if (!path) { reason = 2; permanent = true; }
                    else if (!bytes) { reason = 3; }
                    else if (!vitaSpriteHeaderDimensions(*bytes, entryWidth, entryHeight)) { reason = 4; permanent = true; }
                    int width = 0, height = 0;
                    if (bytes && vitaSpriteHeaderDimensions(*bytes, width, height)
                        && uint64_t(width) * height * 4 <= ReadyLimit)
                    {
                        entry->path = cachePath(hashBytes(bytes->data(), bytes->size(), &options));
                        image = preparedRead();
                        localRead = (SDL_GetTicksNS() - begin) / 1000;
                        if (!image)
                        {
                            begin = SDL_GetTicksNS();
                            {
                                std::unique_lock lock(mutex);
                                const uint64_t decodeBytes = uint64_t(width) * height * 4;
                                wake.wait(lock, [&] { return stop || readyBytes == 0
                                    || readyBytes + decodeBytes <= ReadyQueueLimit; });
                                if (stop) { return; }
                            }
                            Engine::VitaStall::workerStage(6, entry->ticket);
                            image = Engine::decodeImagePixelsBgra(*bytes, *path, options);
                            localDecode = (SDL_GetTicksNS() - begin) / 1000;
                            if (image)
                            {
                                begin = SDL_GetTicksNS();
                                Engine::VitaStall::workerStage(7, entry->ticket);
                                prepareBgraTexturePixelsForUploadInPlace(image->width, image->height,
                                    image->pixels, TextureFilterProfile::Billboard);
                                localBleed = (SDL_GetTicksNS() - begin) / 1000;
                                // Publish first. A shared immutable payload remains alive for persistence
                                // even if the renderer consumes its copy while the worker writes.
                            }
                        }
                        else { hit = true; }
                    }
                }
                else { hit = true; localRead = (SDL_GetTicksNS() - begin) / 1000; }
                if (image)
                {
                    if (allowBc3 && !image->bc3)
                    {
                        Engine::VitaStall::workerStage(11, entry->ticket);
                        compressVitaSprite(*image, pool);
                        upgraded = image->bc3;
                    }
                    delivery = std::make_shared<VitaSpriteImage>(std::move(*image));
                }
            }
            catch (const std::bad_alloc &) { if (!reason) reason = 1; }
            catch (...) { if (!reason) reason = 5; }
            if (!delivery) Engine::VitaStall::workerStage(10, reason ? reason : 6);
            Engine::VitaStall::workerStage(8, entry->ticket);
            {
                std::lock_guard lock(mutex);
                setupUs += localSetup; readyWaitUs += localWait; preparedUs += localPrepared;
                readUs += localRead; decodeUs += localDecode; bleedUs += localBleed; writeUs += localWrite;
                bc3Hits += delivery && delivery->bc3;
                catalogueHits += catalogueHit;
                hits += hit; decoded += bool(delivery) && !hit; failures += !delivery;
                entry->pending = false;
                entry->failed = !delivery && permanent;
                entry->failureReason = delivery ? 0 : (reason ? reason : 6);
                if (!delivery && !permanent)
                {
                    entry->attempts = std::min(entry->attempts + 1, 5u);
                    entry->retryAfter = SDL_GetTicksNS() + (250000000ull << (entry->attempts - 1));
                    ++retries;
                }
                if (delivery)
                {
                    entry->attempts = 0; entry->retryAfter = 0;
                    entry->payloadBytes = delivery->pixels.size() + delivery->maskBits.size();
                    entry->backing = std::max<uint64_t>(delivery->bc3 ? 16 : 4, vitaSpriteBackingBytes(delivery->width, delivery->height) / (delivery->bc3 ? 4 : 1));
                    readyBytes += delivery->pixels.size() + delivery->maskBits.size(); readyPeak = std::max(readyPeak, readyBytes);
                    entry->ready = delivery;
                    entry->readyObservedFrame = 0;
                    entry->offered = false;
                    // Intrusive ready list: no allocation during completion under memory pressure.
                    // Keep original smallest-ticket priority without scanning the full catalogue.
                    Entry **link = &readyHead;
                    while (*link && (*link)->ticket < entry->ticket) { link = &(*link)->nextReady; }
                    entry->nextReady = *link;
                    *link = entry;
                }
            }
            // Optional disk work waits for an empty delivery queue, with bounded retained pixels.
            if (delivery)
            {
                try
                {
                    if (!hit || upgraded)
                    {
                        const bool queued = persistence.push(entry, delivery,
                            delivery->pixels.capacity() + delivery->maskBits.capacity());
                        Engine::VitaStall::activity(queued ? Engine::VitaStall::CacheWriteQueued : Engine::VitaStall::CacheWriteSkipped);
                    }
                    if (catalogue) catalogue->remember(entry->name, entry->palette, entry->path, *delivery);
                }
                catch (...) { std::lock_guard lock(mutex); ++writesFailed; }
            }
            Engine::VitaStall::workerStage(0);
        }
    }
};

VitaSpriteStream::VitaSpriteStream(const Engine::AssetFileSystem &files, std::string world)
    : m_state(std::make_unique<State>(files, std::move(world))) {}
VitaSpriteStream::~VitaSpriteStream() = default;
void VitaSpriteStream::touch(uint32_t ticket, uint64_t frame)
{
    std::lock_guard lock(m_state->mutex);
    if (ticket && ticket <= m_state->entries.size()) { m_state->entries[ticket - 1].touched = frame; }
}
const VitaSpriteImage *VitaSpriteStream::request(uint32_t &ticket, const std::string &name,
    int16_t palette, uint64_t frame)
{
    std::lock_guard lock(m_state->mutex);
    if (!ticket)
    {
        m_state->entries.emplace_back(); ticket = uint32_t(m_state->entries.size());
        auto &entry = m_state->entries.back(); entry.name = name; entry.palette = palette; entry.ticket = ticket;
    }
    auto &entry = m_state->entries[ticket - 1]; entry.touched = frame;
    if (entry.ready) { entry.offered = true; return &*entry.ready; }
    if (!entry.pending && !entry.failed && (!entry.retryAfter || SDL_GetTicksNS() >= entry.retryAfter))
    {
        if (m_state->queue.size() < QueueLimit)
        {
            entry.pending = true; m_state->queue.push_back(&entry); m_state->wake.notify_one();
            OY_VIS(Engine::VitaVisibility::queued());
        }
        else { ++m_state->queueFull; }
    }
    Engine::VitaInitialView::missing(entry.failed);
    return nullptr;
}
bool VitaSpriteStream::failed(uint32_t ticket)
{
    std::lock_guard lock(m_state->mutex);
    return ticket && ticket <= m_state->entries.size() && m_state->entries[ticket - 1].failed;
}
uint32_t VitaSpriteStream::readyTicket()
{
    std::lock_guard lock(m_state->mutex);
    if (m_state->readyHead) { m_state->readyHead->offered = true; }
    return m_state->readyHead ? m_state->readyHead->ticket : 0;
}
void VitaSpriteStream::deferReady(uint32_t ticket)
{
    std::lock_guard lock(m_state->mutex);
    auto *entry = m_state->readyHead;
    if (!entry || entry->ticket != ticket || !entry->nextReady) return;
    m_state->readyHead = entry->nextReady;
    auto **tail = &m_state->readyHead;
    while (*tail) tail = &(*tail)->nextReady;
    entry->nextReady = nullptr;
    *tail = entry;
}
void VitaSpriteStream::consumed(uint32_t ticket)
{
    std::shared_ptr<VitaSpriteImage> retired; // Recycle after state lock, after bgfx::copy/mask copy.
    std::lock_guard lock(m_state->mutex);
    auto &entry = m_state->entries[ticket - 1];
    if (entry.ready)
    {
        State::Entry **link = &m_state->readyHead;
        while (*link && *link != &entry) { link = &(*link)->nextReady; }
        if (*link) { *link = entry.nextReady; }
        entry.nextReady = nullptr;
        m_state->readyBytes -= entry.ready->pixels.size() + entry.ready->maskBits.size();
        retired = std::move(entry.ready);
        entry.ready.reset();
        ++m_state->consumed;
    }
    m_state->wake.notify_one();
}
void VitaSpriteStream::report(uint64_t frame)
{
    Engine::VitaStall::publishPool(1, *m_state->pool);
    std::array<std::shared_ptr<VitaSpriteImage>,32> retired;
    size_t retiredCount=0;
    std::lock_guard lock(m_state->mutex);
    // Expiration only needs ready payloads, not every sprite encountered since map load.
    State::Entry **link = &m_state->readyHead;
    while (*link)
    {
        State::Entry &entry = **link;
        // First observation starts the delivery grace period. A slow read must not
        // turn a newly completed image into an expired one before any upload attempt.
        if (!entry.readyObservedFrame) entry.readyObservedFrame = frame;
        if (entry.offered && frame > entry.touched + 60 && frame > entry.readyObservedFrame + 60
            && retiredCount < retired.size())
        {
            *link = entry.nextReady;
            entry.nextReady = nullptr;
            m_state->readyBytes -= entry.ready->pixels.size() + entry.ready->maskBits.size();
            retired[retiredCount++] = std::move(entry.ready);
            entry.ready.reset();
            ++m_state->expired;
        }
        else { link = &entry.nextReady; }
    }
#if defined(OPENYAMM_VITA_PROFILE)
    uint64_t workingSet = 0, unknown = 0, requests = 0;
    for (const auto &entry : m_state->entries)
    {
        if (frame <= entry.touched + 60)
        {
            workingSet += entry.backing; unknown += entry.backing == 0; ++requests;
        }
    }
#endif
    m_state->wake.notify_one();
    if (auto *progress = Engine::VitaStall::progress)
    {
        const uint32_t values[20] = {uint32_t(reinterpret_cast<uintptr_t>(m_state.get())), uint32_t(frame),
            uint32_t(m_state->queue.size()), uint32_t(m_state->readyBytes), uint32_t(m_state->hits),
            uint32_t(m_state->decoded), uint32_t(m_state->failures), uint32_t(m_state->consumed),
            uint32_t(m_state->expired), uint32_t(m_state->queueFull), uint32_t(m_state->readUs),
            m_state->readyHead ? m_state->readyHead->ticket : 0, uint32_t(m_state->setupUs),
            uint32_t(m_state->readyWaitUs), uint32_t(m_state->preparedUs), uint32_t(m_state->catalogueHits), uint32_t(m_state->retries),
            uint32_t(m_state->writeUs), uint32_t(m_state->writesFailed), uint32_t(m_state->bc3Hits)};
        __atomic_fetch_add(&progress->streamSequence, 1, __ATOMIC_ACQ_REL);
        for (unsigned i=0;i<20;++i) __atomic_store_n(&progress->streamTotals[i], values[i], __ATOMIC_RELAXED);
        __atomic_fetch_add(&progress->streamSequence, 1, __ATOMIC_RELEASE);
    }
#if defined(OPENYAMM_VITA_PROFILE)
    if (Engine::VitaProfile::frame.active)
    {
        Engine::VitaProfile::print("PROFILE STREAM n=%u window_frames=60 requested=%llu working_bytes=%llu "
            "unknown=%llu budget=%u queued=%u ready=%u ready_peak=%u hits=%llu decoded=%llu failed=%llu "
            "write_failed=%llu queue_full=%llu worker_read_us=%llu worker_decode_us=%llu worker_bleed_us=%llu "
            "worker_write_us=%llu bc3_hits=%llu\n", Engine::VitaProfile::frame.index + 1,
            (unsigned long long)requests, (unsigned long long)workingSet, (unsigned long long)unknown, VitaAnimationBudget::Limit,
            unsigned(m_state->queue.size()), unsigned(m_state->readyBytes), unsigned(m_state->readyPeak),
            (unsigned long long)m_state->hits, (unsigned long long)m_state->decoded,
            (unsigned long long)m_state->failures, (unsigned long long)m_state->writesFailed,
            (unsigned long long)m_state->queueFull, (unsigned long long)m_state->readUs,
            (unsigned long long)m_state->decodeUs, (unsigned long long)m_state->bleedUs,
            (unsigned long long)m_state->writeUs, (unsigned long long)m_state->bc3Hits);
    }
#endif
}
}

#else
namespace OpenYAMM::Game
{
struct VitaSpriteStream::State {};
VitaSpriteStream::VitaSpriteStream(const Engine::AssetFileSystem &, std::string) {}
VitaSpriteStream::~VitaSpriteStream() = default;
void VitaSpriteStream::touch(uint32_t, uint64_t) {}
const VitaSpriteImage *VitaSpriteStream::request(uint32_t &, const std::string &, int16_t, uint64_t)
{ return nullptr; }
void VitaSpriteStream::consumed(uint32_t) {}
bool VitaSpriteStream::failed(uint32_t) { return true; }
uint32_t VitaSpriteStream::readyTicket() { return 0; }
void VitaSpriteStream::deferReady(uint32_t) {}
void VitaSpriteStream::report(uint64_t) {}
}
#endif
