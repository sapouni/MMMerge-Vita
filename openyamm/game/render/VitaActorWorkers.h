#pragma once
#include <algorithm>
#include <condition_variable>
#if defined(OPENYAMM_VITA_LOADER)
#include <SDL3/SDL_hints.h>
#endif
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>

namespace OpenYAMM::Game
{
// Persistent helpers plus the calling game thread. Jobs read a complete immutable step snapshot and own disjoint output slots.
// Shared-world side effects are committed by the calling game thread after the barrier.
class VitaActorWorkers
{
public:
    explicit VitaActorWorkers(unsigned count)
    {
        count = std::min(3u, std::max(2u, count));
        participants = count;
        workers.reserve(count - 1);
        try
        {
            for (unsigned i = 1; i < count; ++i) workers.emplace_back([this, i] { run(i); });
        }
        catch (...) { stopAndJoin(); throw; }
    }
    ~VitaActorWorkers() { stopAndJoin(); }
    struct Timing { double workerMs[3]{}, lockMs[3]{}, barrierMs = 0; } timing;
    // Bind the caller's storage before entering another thread. Referring directly to a
    // static thread_local variable inside a lambda resolves the worker's own instance.
    // execute joins every task before the caller may resize or release this storage.
    template<class Container, class Task>
    void executeInto(Container &results, Task task)
    {
        auto *pResults = results.data();
        execute(results.size(), [pResults, task = std::move(task)](size_t index) mutable
        {
            task(index, pResults[index]);
        });
    }
    void execute(size_t size, std::function<void(size_t)> task)
    {
        const auto begin = Clock::now();
        std::unique_lock lock(mutex);
        job = std::move(task); items = size; completed = 0; failure = {}; timing = {}; ++epoch;
        wake.notify_all();
        lock.unlock();
        const auto mainStarted = Clock::now();
        std::exception_ptr mainFailure;
        try { for (size_t i = 0; i < size / participants; ++i) job(i); }
        catch (...) { mainFailure = std::current_exception(); }
        const auto mainFinished = Clock::now();
        lock.lock();
        timing.workerMs[0] = milliseconds(mainFinished - mainStarted);
        timing.lockMs[0] = milliseconds(Clock::now() - mainFinished);
        if (mainFailure && !failure) failure = mainFailure;
        done.wait(lock, [&] { return completed == workers.size(); });
        timing.barrierMs = milliseconds(Clock::now() - begin);
        job = {};
        if (failure) std::rethrow_exception(failure);
    }
private:
    using Clock = std::chrono::steady_clock;
    template<class Duration> static double milliseconds(Duration d)
    { return std::chrono::duration<double, std::milli>(d).count(); }
    void stopAndJoin()
    {
        { std::lock_guard lock(mutex); stopping = true; }
        wake.notify_all();
        for (auto &worker : workers) if (worker.joinable()) worker.join();
    }
    void run(unsigned index)
    {
#if defined(OPENYAMM_VITA_LOADER)
        const char *roles[] = {"actor0", "actor1", "actor2"};
        SDL_SetHint("openyamm_vita_thread_role", roles[index]);
#endif
        unsigned seen = 0;
        for (;;)
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping || epoch != seen; });
            if (stopping) return;
            seen = epoch;
            const size_t begin = items * index / participants, end = items * (index + 1) / participants;
            lock.unlock();
            const auto started = Clock::now();
            std::exception_ptr error;
            try { for (size_t i = begin; i < end; ++i) job(i); }
            catch (...) { error = std::current_exception(); }
            const auto finished = Clock::now();
            lock.lock();
            timing.lockMs[index] = milliseconds(Clock::now() - finished);
            timing.workerMs[index] = milliseconds(finished - started);
            if (error && !failure) failure = error;
            ++completed;
            done.notify_one();
        }
    }
    std::mutex mutex;
    std::condition_variable wake, done;
    bool stopping = false;
    unsigned epoch = 0, participants = 0;
    size_t items = 0, completed = 0;
    std::function<void(size_t)> job;
    std::exception_ptr failure;
    std::vector<std::thread> workers;
};
}
