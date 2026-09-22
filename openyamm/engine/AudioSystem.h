#include "engine/CacheRetirement.h"
#pragma once
#include "engine/BufferPool.h"

#include "engine/AssetFileSystem.h"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OpenYAMM::Engine
{
class AudioSystem
{
public:
    struct CacheStats
    {
        size_t clipCount = 0;
        size_t sampleBytes = 0;
    };

    struct ListenerState
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct PlaybackOptions
    {
        float volume = 1.0f;
        bool positional = false;
        bool loop = false;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    AudioSystem();
    ~AudioSystem();

    AudioSystem(const AudioSystem &) = delete;
    AudioSystem &operator=(const AudioSystem &) = delete;

    bool initialize(const AssetFileSystem &assetFileSystem);
    void shutdown();

    bool preloadClip(const std::string &virtualPath);
    bool registerClip(const std::string &virtualPath, std::vector<float> samples, uint32_t frameCount = 0);
    bool releaseClip(const std::string &virtualPath);
    CacheStats cacheStats() const;
    uint64_t playClip(const std::string &virtualPath, const PlaybackOptions &options);
    void stopClip(uint64_t instanceId);
    void pauseClip(uint64_t instanceId);
    void resumeClip(uint64_t instanceId);
    void setClipVolume(uint64_t instanceId, float volume);
    bool isClipPlaying(uint64_t instanceId) const;
    void clearQueuedAudio();
    void stopAll();
    void update(const ListenerState &listenerState);

private:
    struct AudioClip
    {
        std::shared_ptr<BufferPool<float>> recycler;
        ~AudioClip() { if(recycler) recycler->give(std::move(samples)); }
        std::string virtualPath;
        std::vector<float> samples;
        uint32_t frameCount = 0;
        uint64_t lastUse = 0;
    };

    struct PlayingInstance
    {
        uint64_t instanceId = 0;
        std::shared_ptr<AudioClip> pClip;
        uint32_t frameOffset = 0;
        float volume = 1.0f;
        bool positional = false;
        bool loop = false;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool paused = false;
        std::string pendingPath;
    };

    using RetiredVoices = std::array<std::optional<PlayingInstance>, 16>;
    void retireFinishedVoices(RetiredVoices &retired);

    static constexpr int OutputSampleRate = 48000;
    static constexpr int OutputChannels = 2;
    static constexpr SDL_AudioFormat OutputFormat = SDL_AUDIO_F32LE;
    static constexpr int MixChunkFrames = 1024;
    static constexpr int TargetQueuedBytes = MixChunkFrames * OutputChannels * sizeof(float) * 3;

    using ClipCache = std::unordered_map<std::string, std::shared_ptr<AudioClip>>;
    using RetiredClips = CacheRetirement<ClipCache>;
    std::shared_ptr<AudioClip> loadClip(const std::string &virtualPath, RetiredClips &retired);
    bool hasMixableInstances() const;
    bool mixNextChunk(const ListenerState &listenerState, RetiredVoices &retired);
    void prepareNextChunk(const ListenerState &listenerState, RetiredVoices &retired);
    static void calculateStereoGains(
        const ListenerState &listenerState,
        const PlayingInstance &instance,
        float &leftGain,
        float &rightGain);

    std::shared_ptr<BufferPool<float>> m_samplePool = std::make_shared<BufferPool<float>>(6*1024*1024);
    const AssetFileSystem *m_pAssetFileSystem = nullptr;
    SDL_AudioStream *m_pAudioStream = nullptr;
    SDL_AudioSpec m_outputSpec = {};
    uint64_t m_nextInstanceId = 1;
    uint64_t m_clipUseClock = 0;
    void trimClipCache(size_t targetBytes, RetiredClips &retired);
    std::shared_ptr<AudioClip> decodeClip(const std::string &virtualPath);
    void startWorkers();
    void stopWorkers();
    void decodeLoop();
    void mixLoop();
    mutable std::recursive_mutex m_audioMutex;
    // Queue I/O must never hold the gameplay-facing state mutex. Output users
    // release this lock before acquiring m_audioMutex (no reverse lock order).
    std::mutex m_outputMutex;
    uint64_t m_outputGeneration = 0;
    std::condition_variable_any m_decodeWake;
    std::deque<std::string> m_decodeQueue;
    std::unordered_set<std::string> m_decodePending;
    std::thread m_decodeThread, m_mixThread;
    bool m_workersStop = true;
    bool m_asyncAudio = false;
    ListenerState m_listener;
    size_t m_clipCacheBytes = 0;
    std::array<float, MixChunkFrames * OutputChannels> m_mixSamples{};
    std::unordered_map<std::string, std::shared_ptr<AudioClip>> m_clipCache;

    std::unordered_set<std::string> m_playbackCacheMissWarningPaths;
    std::vector<PlayingInstance> m_playingInstances;
};
}
