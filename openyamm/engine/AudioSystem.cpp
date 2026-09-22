#include "engine/VitaStall.h"
#include <optional>
#include "engine/VitaFrameProfile.h"
#include "engine/AudioSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <chrono>
#include <cstdio>

namespace OpenYAMM::Engine
{
namespace
{
constexpr float GlobalSoundGain = 1.0f / 3.0f;

bool isAudioSubsystemInitialized()
{
    return (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0;
}

SDL_AudioSpec buildOutputSpec()
{
    SDL_AudioSpec spec = {};
    spec.format = SDL_AUDIO_F32LE;
    spec.channels = 2;
    spec.freq = 48000;
    return spec;
}
}

AudioSystem::AudioSystem()
    : m_outputSpec(buildOutputSpec())
{
}

AudioSystem::~AudioSystem()
{
    shutdown();
}

bool AudioSystem::initialize(const AssetFileSystem &assetFileSystem)
{
    if (VitaProfile::noAudio()) { shutdown(); return true; }

#ifdef OPENYAMM_VITA_LOADER
    std::cerr << "STARTUP t=" << SDL_GetTicks() << ": engine audio reset\n";
#endif
    shutdown();
    m_pAssetFileSystem = &assetFileSystem;
#if defined(OPENYAMM_VITA_LOADER)
    m_asyncAudio = true;
    if (FILE *file = std::fopen("/data/vita-performance.ini", "r"))
    {
        char line[160]; unsigned enabled = 1;
        while (std::fgets(line, sizeof(line), file))
            if (std::sscanf(line, " audio_async = %u", &enabled) == 1 && enabled <= 1)
                m_asyncAudio = enabled != 0;
        std::fclose(file);
    }
#endif

    if (!isAudioSubsystemInitialized())
    {
#ifdef OPENYAMM_VITA_LOADER
        std::cerr << "STARTUP t=" << SDL_GetTicks() << ": SDL audio subsystem initialization\n";
#endif
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            std::cerr << "AudioSystem: SDL_InitSubSystem(SDL_INIT_AUDIO) failed: " << SDL_GetError() << '\n';
            return false;
        }
    }

#ifdef OPENYAMM_VITA_LOADER
    std::cerr << "STARTUP t=" << SDL_GetTicks() << ": opening SDL audio stream\n";
#endif
    m_pAudioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &m_outputSpec, nullptr, nullptr);

    if (m_pAudioStream == nullptr)
    {
        std::cerr << "AudioSystem: SDL_OpenAudioDeviceStream failed: " << SDL_GetError() << '\n';
        m_pAssetFileSystem = nullptr;
        return false;
    }

#ifdef OPENYAMM_VITA_LOADER
    std::cerr << "STARTUP t=" << SDL_GetTicks() << ": resuming SDL audio stream\n";
#endif
    if (!SDL_ResumeAudioStreamDevice(m_pAudioStream))
    {
        std::cerr << "AudioSystem: SDL_ResumeAudioStreamDevice failed: " << SDL_GetError() << '\n';
        SDL_DestroyAudioStream(m_pAudioStream);
        m_pAudioStream = nullptr;
        m_pAssetFileSystem = nullptr;
        return false;
    }

#ifdef OPENYAMM_VITA_LOADER
    std::cerr << "STARTUP t=" << SDL_GetTicks() << ": SDL audio ready\n";
#endif
    startWorkers();
    return true;
}

void AudioSystem::trimClipCache(size_t targetBytes, RetiredClips &retired)
{
#if defined(OPENYAMM_VITA_LOADER)
    retireUnpinnedClips(m_clipCache, m_clipCacheBytes, targetBytes, retired);
#else
    (void)targetBytes; (void)retired;
#endif
}

bool AudioSystem::preloadClip(const std::string &virtualPath)
{
    RetiredClips retired; // Destroy cache nodes only after audioLock is released.

    std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
    const std::shared_ptr<AudioClip> pClip = loadClip(virtualPath, retired);
    return pClip != nullptr && pClip->frameCount > 0;
}

bool AudioSystem::registerClip(const std::string &virtualPath, std::vector<float> samples, uint32_t frameCount)
{
    RetiredClips retired; // Destroy cache nodes only after audioLock is released.

    std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
    if (virtualPath.empty() || samples.empty() || samples.size() % OutputChannels != 0
        || samples.size() / OutputChannels > UINT32_MAX
        || frameCount > samples.size() / OutputChannels)
    {
        return false;
    }

    std::shared_ptr<AudioClip> pClip = std::make_shared<AudioClip>();
    pClip->virtualPath = virtualPath;
    pClip->samples = std::move(samples);
    pClip->frameCount = frameCount != 0
        ? frameCount
        : static_cast<uint32_t>(pClip->samples.size() / OutputChannels);

    if (pClip->frameCount == 0)
    {
        return false;
    }

    pClip->lastUse = ++m_clipUseClock;
    if (const auto old = m_clipCache.find(virtualPath); old != m_clipCache.end()) {
        m_clipCacheBytes -= old->second->samples.capacity() * sizeof(float);
        retired.extract(m_clipCache, old);
    }
    m_clipCacheBytes += pClip->samples.capacity() * sizeof(float);
    m_clipCache[virtualPath] = pClip;
    return true;
}

bool AudioSystem::releaseClip(const std::string &virtualPath)
{
    RetiredClips retired; // Destroy cache nodes only after audioLock is released.

    std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
    const auto found = m_clipCache.find(virtualPath);
    if (found == m_clipCache.end()) return false;
    m_clipCacheBytes -= found->second->samples.capacity() * sizeof(float);
    retired.extract(m_clipCache, found);
    return true;
}

AudioSystem::CacheStats AudioSystem::cacheStats() const
{
    std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
    CacheStats stats = {};
    stats.clipCount = m_clipCache.size();

    stats.sampleBytes = m_clipCacheBytes;

    return stats;
}

void AudioSystem::shutdown()
{
    stopAll();
    m_clipCache.clear();
    m_clipCacheBytes = 0;
    m_playbackCacheMissWarningPaths.clear();

    if (m_pAudioStream != nullptr)
    {
        if (isAudioSubsystemInitialized())
        {
            SDL_DestroyAudioStream(m_pAudioStream);
        }

        m_pAudioStream = nullptr;
    }

    m_pAssetFileSystem = nullptr;
    m_nextInstanceId = 1;
}

uint64_t AudioSystem::playClip(const std::string &virtualPath, const PlaybackOptions &options)
{
    RetiredClips retired; // Destroy cache nodes only after audioLock is released.

    std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
    std::shared_ptr<AudioClip> pClip;
    startWorkers();
    if (m_asyncAudio)
    {
        if (!m_pAudioStream || !m_pAssetFileSystem || virtualPath.empty()) return 0;
        startWorkers();
        const auto found = m_clipCache.find(virtualPath);
        if (found != m_clipCache.end())
        {
            pClip = found->second;
            pClip->lastUse = ++m_clipUseClock;
        }
        else
        {
            // Bound outstanding events and unique reads. Reserve the playback ID below so
            // cancellation, pause and volume changes also apply before the asset is ready.
            size_t pendingCount = 0;
            for (const PlayingInstance &instance : m_playingInstances)
                pendingCount += !instance.pendingPath.empty();
            if (pendingCount >= 64) return 0;
            if (!m_decodePending.contains(virtualPath))
            {
                if (m_decodePending.size() >= 16) return 0;
                VitaStall::activity(VitaStall::NewSoundDecode);
                m_decodeQueue.push_back(virtualPath);
                m_decodePending.insert(virtualPath);
                m_decodeWake.notify_one();
            }
        }
    }
    else
    {
    const bool playbackCacheMiss =
        m_pAssetFileSystem != nullptr && !virtualPath.empty() && !m_clipCache.contains(virtualPath);
    const uint64_t loadBeginTickNanoseconds = playbackCacheMiss ? SDL_GetTicksNS() : 0;
    try { pClip = loadClip(virtualPath, retired); }
    catch (const std::bad_alloc &)
    {
        trimClipCache(0, retired);
        std::cerr << "Audio allocation failed, skipped clip: " << virtualPath << '\n';
        return 0;
    }

    if (playbackCacheMiss && m_playbackCacheMissWarningPaths.insert(virtualPath).second)
    {
        std::cerr << "[AssetLoadWarning] kind=audio phase=play"
                  << " path=\"" << virtualPath << "\""
                  << " result=" << (pClip != nullptr ? "loaded" : "failed")
                  << " load_us=" << (SDL_GetTicksNS() - loadBeginTickNanoseconds) / 1000
                  << " frames=" << (pClip != nullptr ? pClip->frameCount : 0)
                  << '\n';
    }

    if (pClip == nullptr || pClip->frameCount == 0)
    {
        return 0;
    }

    }

    PlayingInstance instance = {};
    instance.instanceId = m_nextInstanceId++;
    if (!pClip) instance.pendingPath = virtualPath;
    instance.pClip = std::move(pClip);
    instance.volume = std::clamp(options.volume * GlobalSoundGain, 0.0f, 2.0f);
    instance.positional = options.positional;
    instance.loop = options.loop;
    instance.x = options.x;
    instance.y = options.y;
    instance.z = options.z;
    m_playingInstances.push_back(std::move(instance));
    return m_playingInstances.back().instanceId;
}

void AudioSystem::stopClip(uint64_t instanceId)
{
    std::optional<PlayingInstance> stopped;
    std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
    auto found = std::find_if(m_playingInstances.begin(), m_playingInstances.end(),
        [instanceId](const PlayingInstance &instance) { return instance.instanceId == instanceId; });
    if (found != m_playingInstances.end()) {
        stopped.emplace(std::move(*found));
        m_playingInstances.erase(found);
    }
}

void AudioSystem::pauseClip(uint64_t instanceId)
{
    std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
    for (PlayingInstance &instance : m_playingInstances)
    {
        if (instance.instanceId == instanceId)
        {
            instance.paused = true;
            return;
        }
    }
}

void AudioSystem::resumeClip(uint64_t instanceId)
{
    std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
    for (PlayingInstance &instance : m_playingInstances)
    {
        if (instance.instanceId == instanceId)
        {
            instance.paused = false;
            return;
        }
    }
}

void AudioSystem::setClipVolume(uint64_t instanceId, float volume)
{
    std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
    for (PlayingInstance &instance : m_playingInstances)
    {
        if (instance.instanceId != instanceId)
        {
            continue;
        }

        instance.volume = std::clamp(volume * GlobalSoundGain, 0.0f, 2.0f);
        return;
    }
}

bool AudioSystem::isClipPlaying(uint64_t instanceId) const
{
    std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
    return std::any_of(
        m_playingInstances.begin(),
        m_playingInstances.end(),
        [instanceId](const PlayingInstance &instance)
        {
            return instance.instanceId == instanceId;
        });
}

void AudioSystem::clearQueuedAudio()
{
    std::lock_guard<std::mutex> outputLock(m_outputMutex);
    ++m_outputGeneration;
    if (m_pAudioStream != nullptr && isAudioSubsystemInitialized())
    {
        SDL_ClearAudioStream(m_pAudioStream);
    }
}

void AudioSystem::stopAll()
{
    stopWorkers();
    std::vector<PlayingInstance> stopped;
    {
        std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
        m_decodeQueue.clear(); m_decodePending.clear();
        stopped.swap(m_playingInstances);
    }
    // Last owners of uncached sounds are also released without the state mutex.
    stopped.clear();
    {
        RetiredClips retired;
        std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
        trimClipCache(0, retired);
    }
    clearQueuedAudio();
}

void AudioSystem::update(const ListenerState &listenerState)
{
    RetiredVoices retiredVoices; // Declared before state lock: final owners release after unlock.
    VitaStall::publishPool(0, *m_samplePool);
    RetiredClips retired; // Destroy cache nodes only after audioLock is released.

    std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
    if (m_pAudioStream == nullptr)
    {
        return;
    }

    m_listener = listenerState;
    startWorkers();
    if (m_asyncAudio)
    {
        startWorkers();
        return;
    }
    trimClipCache(6 * 1024 * 1024, retired);
    while (SDL_GetAudioStreamQueued(m_pAudioStream) < TargetQueuedBytes)
    {
        if (!hasMixableInstances())
        {
            break;
        }

        if (!mixNextChunk(listenerState, retiredVoices)) break;
    }
}

std::shared_ptr<AudioSystem::AudioClip> AudioSystem::loadClip(const std::string &virtualPath, RetiredClips &retired)
{
    if (VitaProfile::noAudio()) { return nullptr; }

    if (m_pAssetFileSystem == nullptr || virtualPath.empty())
    {
        return nullptr;
    }

    const std::unordered_map<std::string, std::shared_ptr<AudioClip>>::const_iterator existingClipIt =
        m_clipCache.find(virtualPath);

    if (existingClipIt != m_clipCache.end())
    {
        existingClipIt->second->lastUse = ++m_clipUseClock;
        return existingClipIt->second;
    }

    trimClipCache(4 * 1024 * 1024, retired);
    std::shared_ptr<AudioClip> pClip = decodeClip(virtualPath);
    if (pClip)
    {
        pClip->lastUse = ++m_clipUseClock;
        m_clipCache[virtualPath] = pClip;
        m_clipCacheBytes += pClip->samples.capacity() * sizeof(float);
    }
    return pClip;
}

std::shared_ptr<AudioSystem::AudioClip> AudioSystem::decodeClip(const std::string &virtualPath)
{
    std::optional<std::vector<uint8_t>> clipBytes = m_pAssetFileSystem->readBinaryFile(virtualPath);

    if (!clipBytes || clipBytes->empty())
    {
        return nullptr;
    }

    SDL_IOStream *pIoStream = SDL_IOFromConstMem(clipBytes->data(), clipBytes->size());

    if (pIoStream == nullptr)
    {
        std::cerr << "AudioSystem: SDL_IOFromConstMem failed for " << virtualPath << ": " << SDL_GetError() << '\n';
        return nullptr;
    }

    SDL_AudioSpec clipSpec = {};
    Uint8 *pClipData = nullptr;
    Uint32 clipDataLength = 0;

    if (!SDL_LoadWAV_IO(pIoStream, true, &clipSpec, &pClipData, &clipDataLength))
    {
        std::cerr << "AudioSystem: SDL_LoadWAV_IO failed for " << virtualPath << ": " << SDL_GetError() << '\n';
        return nullptr;
    }

    // SDL_LoadWAV_IO closed the input stream and returned its own decoded buffer.
    clipBytes.reset();

    SDL_AudioStream *pConversionStream = SDL_CreateAudioStream(&clipSpec, &m_outputSpec);

    if (pConversionStream == nullptr)
    {
        std::cerr << "AudioSystem: SDL_CreateAudioStream failed for " << virtualPath << ": " << SDL_GetError() << '\n';
        SDL_free(pClipData);
        return nullptr;
    }

    if (!SDL_PutAudioStreamData(pConversionStream, pClipData, static_cast<int>(clipDataLength))
        || !SDL_FlushAudioStream(pConversionStream))
    {
        std::cerr << "AudioSystem: audio conversion queue failed for " << virtualPath << ": " << SDL_GetError()
                  << '\n';
        SDL_DestroyAudioStream(pConversionStream);
        SDL_free(pClipData);
        return nullptr;
    }

    // SDL_PutAudioStreamData copies its input; release the redundant PCM before output allocation.
    SDL_free(pClipData);
    pClipData = nullptr;

    const int convertedBytes = SDL_GetAudioStreamAvailable(pConversionStream);

    if (convertedBytes <= 0 || convertedBytes % (sizeof(float) * OutputChannels) != 0)
    {
        SDL_DestroyAudioStream(pConversionStream);
        SDL_free(pClipData);
        return nullptr;
    }

    std::vector<float> convertedSamples;
    try { convertedSamples = m_samplePool->take(static_cast<size_t>(convertedBytes) / sizeof(float)); }
    catch (...)
    {
        SDL_DestroyAudioStream(pConversionStream);
        SDL_free(pClipData);
        throw;
    }
    const int bytesRead = SDL_GetAudioStreamData(pConversionStream, convertedSamples.data(), convertedBytes);
    SDL_DestroyAudioStream(pConversionStream);
    SDL_free(pClipData);

    if (bytesRead != convertedBytes)
    {
        std::cerr << "AudioSystem: SDL_GetAudioStreamData failed for " << virtualPath << ": " << SDL_GetError()
                  << '\n';
        return nullptr;
    }

    std::shared_ptr<AudioClip> pClip = std::make_shared<AudioClip>();
    pClip->virtualPath = virtualPath;
    pClip->samples = std::move(convertedSamples);
    pClip->recycler = m_samplePool;
    pClip->frameCount = static_cast<uint32_t>(pClip->samples.size() / OutputChannels);
    return pClip;
}

bool AudioSystem::hasMixableInstances() const
{
    return std::any_of(
        m_playingInstances.begin(),
        m_playingInstances.end(),
        [](const PlayingInstance &instance)
        {
            return !instance.paused && instance.pClip != nullptr && instance.pClip->frameCount > 0;
        });
}

bool AudioSystem::mixNextChunk(const ListenerState &listenerState, RetiredVoices &retired)
{
    prepareNextChunk(listenerState, retired);
    return SDL_PutAudioStreamData(m_pAudioStream, m_mixSamples.data(),
        static_cast<int>(m_mixSamples.size() * sizeof(float)));
}

void AudioSystem::prepareNextChunk(const ListenerState &listenerState, RetiredVoices &retired)
{
    VitaProfile::Scope profile(VitaProfile::AudioMix);

    auto &mixedSamples = m_mixSamples;
    mixedSamples.fill(0.0f);

    for (PlayingInstance &instance : m_playingInstances)
    {
        if (instance.paused || instance.pClip == nullptr || instance.pClip->frameCount == 0)
        {
            continue;
        }

        float leftGain = 0.0f;
        float rightGain = 0.0f;
        calculateStereoGains(listenerState, instance, leftGain, rightGain);
        if (leftGain == 0.0f && rightGain == 0.0f)
        {
            const uint64_t advanced = uint64_t(instance.frameOffset) + MixChunkFrames;
            instance.frameOffset = instance.loop
                ? uint32_t((advanced - 1) % instance.pClip->frameCount + 1)
                : uint32_t(std::min(advanced, uint64_t(instance.pClip->frameCount)));
            continue;
        }

        for (int frameIndex = 0; frameIndex < MixChunkFrames; ++frameIndex)
        {
            if (instance.frameOffset >= instance.pClip->frameCount)
            {
                if (!instance.loop)
                {
                    break;
                }

                instance.frameOffset = 0;
            }

            const size_t sourceIndex = static_cast<size_t>(instance.frameOffset) * OutputChannels;
            const size_t destinationIndex = static_cast<size_t>(frameIndex) * OutputChannels;
            mixedSamples[destinationIndex] += instance.pClip->samples[sourceIndex] * leftGain;
            mixedSamples[destinationIndex + 1] += instance.pClip->samples[sourceIndex + 1] * rightGain;
            ++instance.frameOffset;
        }
    }

    for (float &sample : mixedSamples)
    {
        sample = std::clamp(sample, -1.0f, 1.0f);
    }

    retireFinishedVoices(retired);
}

void AudioSystem::retireFinishedVoices(RetiredVoices &retired)
{
    size_t slot=0;
    m_playingInstances.erase(std::remove_if(m_playingInstances.begin(),m_playingInstances.end(),
        [&retired,&slot](PlayingInstance &instance)
        {
            const bool finished=(!instance.pClip && instance.pendingPath.empty())
                || (instance.pClip && !instance.loop && instance.frameOffset>=instance.pClip->frameCount);
            if (!finished) return false;
            while(slot<retired.size() && retired[slot]) ++slot;
            if(slot==retired.size()) return false; // Defer excess cleanup, never drop a live sound.
            retired[slot++].emplace(std::move(instance));
            return true;
        }),m_playingInstances.end());
}


void AudioSystem::startWorkers()
{
    std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
    if (!m_asyncAudio || !m_pAudioStream || !m_workersStop) return;
    m_workersStop = false;
    try
    {
        m_decodeThread = std::thread(&AudioSystem::decodeLoop, this);
        m_mixThread = std::thread(&AudioSystem::mixLoop, this);
    }
    catch (const std::exception &error)
    {
        m_workersStop = true;
        m_asyncAudio = false;
        m_decodeWake.notify_all();
        std::fprintf(stderr, "Audio workers unavailable; synchronous audio retained: %s\n", error.what());
    }
}

void AudioSystem::stopWorkers()
{
    {
        std::lock_guard<std::recursive_mutex> audioLock(m_audioMutex);
        m_workersStop = true;
        m_decodeWake.notify_all();
    }
    if (m_decodeThread.joinable()) m_decodeThread.join();
    if (m_mixThread.joinable()) m_mixThread.join();
}

void AudioSystem::decodeLoop()
{
#if defined(OPENYAMM_VITA_LOADER)
    SDL_SetHint("openyamm_vita_thread_role", "audio_decode");
#endif
    for (;;)
    {
        VitaStall::workerStage(40);
        std::string path;
        {
            RetiredClips retired;
            std::unique_lock<std::recursive_mutex> lock(m_audioMutex);
            m_decodeWake.wait(lock, [this] { return m_workersStop || !m_decodeQueue.empty(); });
            if (m_workersStop) return;
            path = std::move(m_decodeQueue.front());
            m_decodeQueue.pop_front();
            trimClipCache(4 * 1024 * 1024, retired);
        }
        std::shared_ptr<AudioClip> clip;
        VitaStall::workerStage(41);
        try { clip = decodeClip(path); }
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "Audio preparation failed: %s: %s\n", path.c_str(), error.what());
        }
        VitaStall::workerStage(42);
        std::shared_ptr<AudioClip> replacedClip;
        std::lock_guard<std::recursive_mutex> lock(m_audioMutex);
        if (m_workersStop) return;
        m_decodePending.erase(path);
        bool wanted = false;
        for (PlayingInstance &instance : m_playingInstances)
        {
            if (instance.pendingPath != path) continue;
            instance.pClip = clip;
            instance.pendingPath.clear();
            wanted = true;
        }
        // Cancelled reads are discarded; never let an obsolete request restart a sound.
        if (clip && wanted)
        {
            clip->lastUse = ++m_clipUseClock;
            try
            {
                auto [entry, inserted] = m_clipCache.try_emplace(path, clip);
                if (!inserted)
                {
                    m_clipCacheBytes -= entry->second->samples.capacity() * sizeof(float);
                    replacedClip = std::move(entry->second);
                    entry->second = clip;
                }
                m_clipCacheBytes += clip->samples.capacity() * sizeof(float);
            }
            catch (const std::bad_alloc &)
            {
                // Playback already owns the decoded clip. A cache entry is optional.
                std::fprintf(stderr, "Audio cache admission failed: %s\n", path.c_str());
            }
        }
        m_playingInstances.erase(std::remove_if(m_playingInstances.begin(), m_playingInstances.end(),
            [](const PlayingInstance &instance) { return !instance.pClip && instance.pendingPath.empty(); }),
            m_playingInstances.end());
    }
}

void AudioSystem::mixLoop()
{
#if defined(OPENYAMM_VITA_LOADER)
    SDL_SetHint("openyamm_vita_thread_role", "audio_mix");
#endif
    unsigned trimChunks = 0;
    for (;;)
    {
        RetiredClips retired;
        RetiredVoices retiredVoices;

        bool filled = false;
        bool needsChunk;
        uint64_t generation;
        {
            std::lock_guard<std::mutex> outputLock(m_outputMutex);
            generation = m_outputGeneration;
            needsChunk = SDL_GetAudioStreamQueued(m_pAudioStream) < TargetQueuedBytes;
        }
        {
            std::lock_guard<std::recursive_mutex> lock(m_audioMutex);
            if (m_workersStop) return;
            if (needsChunk && hasMixableInstances())
            {
                prepareNextChunk(m_listener, retiredVoices);
                filled = true;
            }
            // Pinned voices can keep the cache above target. Do not rescan and
            // sort the same non-evictable catalogue on every 4 ms poll.
            if (++trimChunks == 250)
            {
                trimClipCache(6 * 1024 * 1024, retired);
                trimChunks = 0;
            }
        }
        if (filled)
        {
            std::lock_guard<std::mutex> outputLock(m_outputMutex);
            // A stop/clear between preparation and submission cancels this chunk.
            if (generation == m_outputGeneration)
                filled = SDL_PutAudioStreamData(m_pAudioStream, m_mixSamples.data(),
                    static_cast<int>(m_mixSamples.size() * sizeof(float)));
        }
        // A successful chunk must yield too: a high-priority producer that
        // cannot fill the queue can otherwise reacquire the state lock forever.
        std::this_thread::sleep_for(std::chrono::milliseconds(filled ? 1 : 4));
    }
}

void AudioSystem::calculateStereoGains(
    const ListenerState &listenerState,
    const PlayingInstance &instance,
    float &leftGain,
    float &rightGain)
{
    if (!instance.positional)
    {
        leftGain = instance.volume;
        rightGain = instance.volume;
        return;
    }

    const float deltaX = instance.x - listenerState.x;
    const float deltaY = instance.y - listenerState.y;
    const float deltaZ = instance.z - listenerState.z;
    const float distance = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
    constexpr float MaxAudibleDistance = 12000.0f;

    if (distance >= MaxAudibleDistance)
    {
        leftGain = 0.0f;
        rightGain = 0.0f;
        return;
    }

    const float normalizedDistance = distance / 768.0f;
    const float attenuation = 1.0f / (1.0f + normalizedDistance * normalizedDistance);
    const float pan = std::clamp(deltaX / 2048.0f, -1.0f, 1.0f);
    leftGain = instance.volume * attenuation * (pan <= 0.0f ? 1.0f : (1.0f - pan));
    rightGain = instance.volume * attenuation * (pan >= 0.0f ? 1.0f : (1.0f + pan));
}
}
