#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include "../../engine/VitaStallProtocol.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include "bridge-allocator.h"
static struct OyStallProgress oy_stall;
static SceUID oy_stall_thread = -1;
static SceUID oy_stall_game_thread = -1;
unsigned oy_stall_capture_enabled;
uint64_t oy_thread_run_ticks(void) {
    SceKernelThreadInfo info = {0}; info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0) return UINT64_MAX;
    return info.runClocks;
}
static uint64_t oy_cpu_probe[4];
static unsigned oy_path_yields, oy_path_max_gap_us;
// Worker-only, outside engine/allocator locks. No SDL properties, heap use or logging.
void oy_path_cooperate(uint64_t *lastYield)
{
    uint64_t now = sceKernelGetProcessTimeWide();
    if (!*lastYield) { *lastYield = now; return; }
    const uint64_t gap = now - *lastYield;
    if (gap < 2000) return;
    if (oy_stall_capture_enabled) {
        __atomic_fetch_add(&oy_path_yields, 1, __ATOMIC_RELAXED);
        oy_allocator_max(&oy_path_max_gap_us, gap > UINT32_MAX ? UINT32_MAX : (unsigned)gap);
    }
    sceKernelDelayThread(1000);
    *lastYield = sceKernelGetProcessTimeWide();
}
void oy_capture_cpu_probe(uint64_t *out) {
    if (!oy_cpu_probe[0]) {
        uint64_t wall = sceKernelGetProcessTimeWide(), cpu = oy_thread_run_ticks();
        while (sceKernelGetProcessTimeWide() - wall < 5000) { __asm__ volatile ("" ::: "memory"); }
        uint64_t done = oy_thread_run_ticks();
        oy_cpu_probe[0] = sceKernelGetProcessTimeWide() - wall;
        oy_cpu_probe[1] = cpu != UINT64_MAX && done != UINT64_MAX && done >= cpu ? done-cpu : UINT64_MAX;
        wall = sceKernelGetProcessTimeWide(); cpu = oy_thread_run_ticks();
        sceKernelDelayThread(5000); done = oy_thread_run_ticks();
        oy_cpu_probe[2] = sceKernelGetProcessTimeWide() - wall;
        oy_cpu_probe[3] = cpu != UINT64_MAX && done != UINT64_MAX && done >= cpu ? done-cpu : UINT64_MAX;
    }
    memcpy(out, oy_cpu_probe, sizeof(oy_cpu_probe));
}
unsigned oy_lock_diagnostics;
static unsigned oy_activity[10];
void oy_capture_activity(unsigned kind)
{
    if (kind < 10 && __atomic_load_n(&oy_stall_capture_enabled, __ATOMIC_RELAXED))
        __atomic_fetch_add(&oy_activity[kind], 1, __ATOMIC_RELAXED);
}
unsigned oy_native_lock_owner(uintptr_t);
void oy_native_lock_summary(uint64_t *);
static struct { SceUID id; unsigned role, stage, stageUs, detail, failure; uintptr_t mutex, caller; } oy_threads[64];
static const char *oy_thread_roles[] = {"guest", "game", "render", "asset", "path", "capture", "video", "audio_mix", "audio_decode"};
void oy_stall_register_thread(const char *role)
{
    const SceUID id = sceKernelGetThreadId(); unsigned code = 0;
    for (unsigned i = 1; i < sizeof(oy_thread_roles)/sizeof(*oy_thread_roles); ++i)
        if (strcmp(role, oy_thread_roles[i]) == 0) { code = i; break; }
    for (unsigned i = 0; i < 64; ++i)
        if (__atomic_load_n(&oy_threads[i].id, __ATOMIC_ACQUIRE) == id)
        { __atomic_store_n(&oy_threads[i].role, code, __ATOMIC_RELEASE); return; }
    for (unsigned i = 0; i < 64; ++i)
    {
        SceUID empty = 0;
        if (__atomic_compare_exchange_n(&oy_threads[i].id, &empty, id, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        { __atomic_store_n(&oy_threads[i].role, code, __ATOMIC_RELEASE); return; }
    }
}
void oy_worker_stage(unsigned stage, unsigned detail)
{
    if (!__atomic_load_n(&oy_stall_capture_enabled, __ATOMIC_RELAXED)) return;
    const SceUID id = sceKernelGetThreadId();
    for (unsigned i=0;i<64;++i) if (__atomic_load_n(&oy_threads[i].id,__ATOMIC_ACQUIRE)==id)
    {
        __atomic_store_n(&oy_threads[i].stageUs,(unsigned)sceKernelGetProcessTimeWide(),__ATOMIC_RELAXED);
        __atomic_store_n(&oy_threads[i].detail,detail,__ATOMIC_RELAXED);
        if (stage==10) __atomic_store_n(&oy_threads[i].failure,detail,__ATOMIC_RELAXED);
        __atomic_store_n(&oy_threads[i].stage,stage,__ATOMIC_RELEASE);
        break;
    }
}
void oy_stall_unregister_thread(void)
{
    const SceUID id = sceKernelGetThreadId();
    for (unsigned i = 0; i < 64; ++i)
    {
        SceUID expected = id;
        __atomic_compare_exchange_n(&oy_threads[i].id, &expected, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
}
static unsigned oy_thread_lookup[128]; // slot+1; validate id before every use (thread IDs can be reused).
void oy_stall_mutex_wait_for(unsigned id, uintptr_t mutex, uintptr_t caller)
{
    unsigned hash=(id*2654435761u)>>25;
    unsigned cached=__atomic_load_n(&oy_thread_lookup[hash],__ATOMIC_RELAXED);
    unsigned slot=64;
    if (cached && __atomic_load_n(&oy_threads[cached-1].id,__ATOMIC_ACQUIRE)==(SceUID)id) slot=cached-1;
    else for(unsigned i=0;i<64;++i) if(__atomic_load_n(&oy_threads[i].id,__ATOMIC_ACQUIRE)==(SceUID)id) {
        slot=i; __atomic_store_n(&oy_thread_lookup[hash],i+1,__ATOMIC_RELAXED); break;
    }
    if (slot<64) {
        __atomic_store_n(&oy_threads[slot].caller,caller,__ATOMIC_RELAXED);
        __atomic_store_n(&oy_threads[slot].mutex,mutex,__ATOMIC_RELEASE);
    }
}
void oy_stall_mutex_wait(uintptr_t mutex, uintptr_t caller)
{ oy_stall_mutex_wait_for((unsigned)sceKernelGetThreadId(),mutex,caller); }
static int oy_stall_attempted, oy_stall_started, oy_stall_start_result;
static unsigned oy_stall_watch_entered, oy_stall_watch_polls;
static unsigned oy_mutex_max_us, oy_mutex_max_thread, oy_mutex_max_caller;
void oy_stall_mutex_completed_for(unsigned id, unsigned elapsed, uintptr_t caller)
{
    if ((SceUID)id == oy_stall_game_thread && elapsed > oy_mutex_max_us) {
        __atomic_store_n(&oy_mutex_max_thread,id,__ATOMIC_RELAXED);
        __atomic_store_n(&oy_mutex_max_caller,(unsigned)caller,__ATOMIC_RELAXED);
        __atomic_store_n(&oy_mutex_max_us,elapsed,__ATOMIC_RELEASE);
    }
}
void oy_stall_mutex_completed(unsigned begin, uintptr_t caller)
{ oy_stall_mutex_completed_for((unsigned)sceKernelGetThreadId(),(unsigned)sceKernelGetProcessTimeWide()-begin,caller); }
static const char *oy_stall_names[] = {
    "frame", "events", "callback", "post_callback", "bgfx",
    "simulation", "facts", "decisions", "apply", "actor_state",
    "movement", "physics", "path_build", "path_resolve", "move_sweep",
    "move_post", "move_facts", "melee", "corpse", "projectiles",
    "player", "world_render", "HUD", "audio", "visibility",
    "lighting", "recovery", "geometry", "path_clear", "incident_write",
    "shared_state", "party_presentation", "party_audio", "shared_input", "movement_frame", "combat_dispatch", "interaction", "gameplay_tail", "hud_state", "hud_assets", "hud_draw"
};
static char oy_stall_batch[32768];
static unsigned oy_stall_batch_size, oy_stall_batching;
static int oy_stall_last_open_error, oy_stall_last_write_error;
// Capture worker calls this; no allocation, FILE handle, heap lock or formatting.
// Counters are cumulative atomic observations, not one instantaneous locked sample.
void oy_capture_health(uint64_t *out, unsigned count)
{
    if (count < 23) return;
#define OY_HEALTH(i, name) out[i] = __atomic_load_n(&name, __ATOMIC_RELAXED)
    OY_HEALTH(0, oy_original_acquires); OY_HEALTH(1, oy_original_max);
    OY_HEALTH(2, oy_original_max_owner); OY_HEALTH(3, oy_original_max_caller);
    OY_HEALTH(4, oy_original_wait_max); OY_HEALTH(5, oy_original_slow);
    OY_HEALTH(6, oy_original_max_sbrks); OY_HEALTH(7, oy_sbrk_calls);
    OY_HEALTH(8, oy_sbrk_grow); OY_HEALTH(9, oy_sbrk_trim);
    OY_HEALTH(10, oy_sbrk_us); OY_HEALTH(11, oy_sbrk_max); OY_HEALTH(12, oy_sbrk_failed);
    OY_HEALTH(13, oy_stall_last_open_error); OY_HEALTH(14, oy_stall_last_write_error);
    OY_HEALTH(15, oy_original_owner); OY_HEALTH(16, oy_original_caller);
    out[17] = sceKernelGetProcessTimeWide();
    out[18] = (uint64_t)(unsigned)__atomic_load_n(&oy_stall_start_result, __ATOMIC_RELAXED)
        | ((uint64_t)__atomic_load_n(&oy_stall_watch_entered, __ATOMIC_ACQUIRE) << 32);
    out[19] = __atomic_load_n(&oy_stall_watch_polls, __ATOMIC_RELAXED);
    out[20] = __atomic_load_n(&oy_mutex_max_us, __ATOMIC_ACQUIRE);
    out[21] = __atomic_load_n(&oy_mutex_max_thread, __ATOMIC_RELAXED);
    out[22] = __atomic_load_n(&oy_mutex_max_caller, __ATOMIC_RELAXED);
#undef OY_HEALTH
}
static void oy_stall_write(const char *line)
{
    if (oy_stall_batching)
    {
        const size_t length = strlen(line);
        if (length < sizeof(oy_stall_batch) - oy_stall_batch_size)
        {
            memcpy(oy_stall_batch + oy_stall_batch_size, line, length);
            oy_stall_batch_size += length;
            oy_stall_batch[oy_stall_batch_size] = 0;
        }
        return;
    }
    // No guest callbacks, malloc, stdio file lock or game-thread cooperation.
    SceUID fd = sceIoOpen("ux0:data/openyamm/stall-01.06.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    __atomic_store_n(&oy_stall_last_open_error, fd < 0 ? fd : 0, __ATOMIC_RELAXED);
    if (fd >= 0)
    {
        unsigned left = strlen(line);
        while (left)
        {
            int n = sceIoWrite(fd, line, left);
            if (n <= 0)
            { __atomic_store_n(&oy_stall_last_write_error, n ? n : -1, __ATOMIC_RELAXED); break; }
            line += n; left -= n;
        }
        sceIoClose(fd);
    }
}
static void oy_stall_pool_counters(void)
{
    static const char *names[] = {"audio", "sprites", "path"};
    char lines[1536]; unsigned used = 0;
    for (unsigned kind=0; kind<3; ++kind)
    {
        const struct OyPoolSnapshot *p=&oy_stall.pools[kind];
        const unsigned before=__atomic_load_n(&p->sequence,__ATOMIC_ACQUIRE);
        if (!before || (before&1)) continue;
        const uint32_t *fields[]={&p->instance,&p->frame,&p->limit,&p->retained,&p->peak,
            &p->hits,&p->misses,&p->returned,&p->rejected,&p->maxRequest};
        unsigned v[10];
        for(unsigned i=0;i<10;++i) v[i]=__atomic_load_n(fields[i],__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(before!=__atomic_load_n(&p->sequence,__ATOMIC_RELAXED)) continue;
        int n=sceClibSnprintf(lines+used,sizeof(lines)-used,
            "POOL build=01.06 kind=%s instance=%x published_frame=%u limit=%u retained=%u peak=%u hits=%u misses=%u returned=%u rejected=%u max_request=%u\n",
            names[kind],v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9]);
        if(n>0 && (unsigned)n<sizeof(lines)-used) used+=(unsigned)n;
    }
    if(used) oy_stall_write(lines);
    const unsigned streamBefore=__atomic_load_n(&oy_stall.streamSequence,__ATOMIC_ACQUIRE);
    if(streamBefore && !(streamBefore&1))
    {
        unsigned v[15];
        for(unsigned i=0;i<15;++i) v[i]=__atomic_load_n(&oy_stall.streamTotals[i],__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(streamBefore==__atomic_load_n(&oy_stall.streamSequence,__ATOMIC_RELAXED))
        {
            sceClibSnprintf(lines,sizeof(lines),
                "STREAM build=01.06 instance=%x published_frame=%u queued=%u ready_bytes=%u hits=%u decoded=%u failed=%u consumed=%u expired=%u queue_full=%u read_us=%u ready_head=%u setup_us=%u ready_wait_us=%u prepared_us=%u\n",
                v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13],v[14]);
            oy_stall_write(lines);
        }
    }
    const unsigned before=__atomic_load_n(&oy_stall.pickSequence,__ATOMIC_ACQUIRE);
    if(before && !(before&1))
    {
        unsigned v[24];
        for(unsigned i=0;i<24;++i) v[i]=__atomic_load_n(&oy_stall.pickTotals[i],__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(before==__atomic_load_n(&oy_stall.pickSequence,__ATOMIC_RELAXED))
        {
            // Cumulative counts: subtract adjacent snapshots and divide by elapsed_ms.
            sceClibSnprintf(lines,sizeof(lines),
                "PICK build=01.06 elapsed_ms=%u frames=%u hover_refreshes=%u context_refreshes=%u context_fallback_queries=%u forced_context=%u fast_turn_frames=%u hover_us=%u context_us=%u keyboard_use_queries=%u\n",
                v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9]);
            oy_stall_write(lines);
            for(unsigned group=0;group<2;++group)
            {
                const unsigned b=10+group*7;
                sceClibSnprintf(lines,sizeof(lines),
                    "PICK_STATIONARY build=01.06 state=%s observed_ms=%u frames=%u hover_refreshes=%u hover_us=%u context_refreshes=%u context_us=%u context_fallback_queries=%u\n",
                    group ? "targetable" : "none",v[b],v[b+1],v[b+2],v[b+3],v[b+4],v[b+5],v[b+6]);
                oy_stall_write(lines);
            }
        }
    }
}
static int oy_stall_watch(SceSize size, void *arg)
{
    (void)size; (void)arg;
    __atomic_store_n(&oy_stall_watch_entered, 1, __ATOMIC_RELEASE);
    oy_stall_write("WATCHDOG_START build=01.06 threshold_ms=3000 pc_sampling=0 allocation_tracking=0 heap_scans=0 coarse_phases=1\n");
    unsigned lines = 0, previousFrame = 0, stalledFrame = 0, polls = 0, poolPolls = 0;
    for (;;)
    {
        sceKernelDelayThread(1000000);
        __atomic_fetch_add(&oy_stall_watch_polls, 1, __ATOMIC_RELAXED);
        unsigned enabled = __atomic_load_n(&oy_stall.enabled, __ATOMIC_ACQUIRE);
        unsigned frame = __atomic_load_n(&oy_stall.frame, __ATOMIC_RELAXED);
        unsigned heartbeat = __atomic_load_n(&oy_stall.heartbeatUs, __ATOMIC_RELAXED);
        unsigned age = (unsigned)sceKernelGetProcessTimeWide() - heartbeat;
        if (!enabled || frame != previousFrame) { polls = 0; }
        previousFrame = frame;
        if (!enabled) { continue; }
        // Aggregate counters only. All formatting/card output stays on watchdog;
        // at most one sub-KiB batch per ten seconds, including normally running frames.
        if (++poolPolls == 10) {
            oy_stall_pool_counters(); poolPolls=0;
            char surfaceLine[192];
            sceClibSnprintf(surfaceLine, sizeof(surfaceLine),
                "SURFACE frame=%u static_opaque_draws=%u static_alpha_draws=%u dynamic_blended_draws=%u\n", frame,
                __atomic_load_n(&oy_stall.opaqueSurfaceDraws, __ATOMIC_RELAXED),
                __atomic_load_n(&oy_stall.alphaSurfaceDraws, __ATOMIC_RELAXED),
                __atomic_load_n(&oy_stall.blendedSurfaceDraws, __ATOMIC_RELAXED));
            oy_stall_write(surfaceLine);
            // Retain short-hitch evidence too; a >3 second watchdog trigger is not required.
            char heapLine[768];
            oy_allocator_snapshot(heapLine, sizeof(heapLine)); oy_stall_write(heapLine);
            for (unsigned i = 0; i < 8; ++i) {
                struct OyHeapSite *site = &oy_heap_sites[i];
                unsigned caller = __atomic_load_n(&site->caller, __ATOMIC_ACQUIRE);
                if (!caller) continue;
                sceClibSnprintf(heapLine, sizeof(heapLine),
                    "HEAP_SITE frame=%u caller=%x count=%u hold_us=%u max_us=%u holds_ge_1ms=%u max_owner=%u\n",
                    frame, caller, __atomic_load_n(&site->count, __ATOMIC_RELAXED),
                    __atomic_load_n(&site->us, __ATOMIC_RELAXED),
                    __atomic_load_n(&site->max_us, __ATOMIC_RELAXED),
                    __atomic_load_n(&site->slow, __ATOMIC_RELAXED),
                    __atomic_load_n(&site->owner, __ATOMIC_RELAXED));
                oy_stall_write(heapLine);
            }
        }
        if (stalledFrame && frame != stalledFrame)
        {
            char line[96]; sceClibSnprintf(line, sizeof(line), "RECOVERED prior_frame=%u now_frame=%u\n", stalledFrame, frame);
            oy_stall_write(line); stalledFrame = 0;
        }
        // Ordinary early hitches consume no slots. Polls also detect very long stalls across clock wrap.
        ++polls;
        if (age < 3000000 && polls < 4) { continue; }
        // First 120 stall snapshots every second, then every 15s; capped at 512 per launch.
        if (lines >= 512 || (lines >= 120 && polls % 15)) { continue; }
        // Snapshot every thread before doing any card writes.
        oy_stall_batch_size = 0; oy_stall_batch[0] = 0; oy_stall_batching = 1;
        unsigned context = __atomic_load_n(&oy_stall.context, __ATOMIC_RELAXED);
        unsigned phase = context >> 24;
        char line[768];
        sceClibSnprintf(line, sizeof(line), "STALL build=01.06 frame=%u frame_age_ms=%u phase=%s actor=%u sector=%d step=%u hz=%u progress=%u detail=%u\n",
            frame, age / 1000, phase < OyStallPhaseCount ? oy_stall_names[phase] : "unknown", context & 0xffffffu,
            (int)__atomic_load_n(&oy_stall.sector, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_stall.step, __ATOMIC_RELAXED), __atomic_load_n(&oy_stall.hz, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_stall.progress, __ATOMIC_RELAXED), __atomic_load_n(&oy_stall.detail, __ATOMIC_RELAXED));
        oy_stall_write(line);
        SceKernelThreadInfo info = {0}; info.size = sizeof(info);
        const SceUID gameThread = __atomic_load_n(&oy_stall_game_thread, __ATOMIC_RELAXED);
        const int query = sceKernelGetThreadInfo(gameThread, &info);
        sceClibSnprintf(line, sizeof(line),
            "THREAD frame=%u query=%x id=%d status=%x wait_type=%u wait_id=%d priority=%d cpu=%d preempt=%u releases=%u\n",
            frame, query, gameThread, info.status, info.waitType, info.waitId, info.currentPriority,
            info.currentCpuId, info.threadPreemptCount, info.threadReleaseCount);
        oy_stall_write(line);
        // Ask the kernel which object this is; do not guess from waitType or
        // dereference a possibly stale guest mutex. Wrong-type queries fail.
        if (query == 0 && info.waitId > 0)
        {
            SceKernelMutexInfo mutex = {0}; mutex.size = sizeof(mutex);
            SceKernelSemaInfo sema = {0}; sema.size = sizeof(sema);
            if (sceKernelGetMutexInfo(info.waitId, &mutex) == 0)
            {
                sceClibSnprintf(line, sizeof(line),
                    "WAIT_OBJECT frame=%u id=%d kind=mutex name=%.31s owner=%d waiters=%d\n",
                    frame, info.waitId, mutex.name, mutex.currentOwnerId, mutex.numWaitThreads);
                oy_stall_write(line);
            }
            else if (sceKernelGetSemaInfo(info.waitId, &sema) == 0)
            {
                sceClibSnprintf(line, sizeof(line),
                    "WAIT_OBJECT frame=%u id=%d kind=semaphore name=%.31s count=%d waiters=%d\n",
                    frame, info.waitId, sema.name, sema.currentCount, sema.numWaitThreads);
                oy_stall_write(line);
            }
        }
        ++lines; stalledFrame = frame;
        for (unsigned i = 0; i < 64; ++i)
        {
            const SceUID id = __atomic_load_n(&oy_threads[i].id, __ATOMIC_ACQUIRE);
            if (id <= 0) { continue; }
            const unsigned role = __atomic_load_n(&oy_threads[i].role, __ATOMIC_ACQUIRE);
            SceKernelThreadInfo worker = {0}; worker.size = sizeof(worker);
            const int result = sceKernelGetThreadInfo(id, &worker);
            sceClibSnprintf(line, sizeof(line),
                "WORKER frame=%u id=%d role=%s query=%x status=%x wait_type=%u wait_id=%d priority=%d cpu=%d preempt=%u releases=%u\n",
                frame, id, role < sizeof(oy_thread_roles)/sizeof(*oy_thread_roles) ? oy_thread_roles[role] : "unknown", result, worker.status,
                worker.waitType, worker.waitId, worker.currentPriority, worker.currentCpuId,
                worker.threadPreemptCount, worker.threadReleaseCount);
            oy_stall_write(line);
            uintptr_t mutex = __atomic_load_n(&oy_threads[i].mutex, __ATOMIC_ACQUIRE);
            if (mutex)
            {
                sceClibSnprintf(line, sizeof(line), "LOCK_WAIT frame=%u id=%d mutex=%p caller=%p\n",
                    frame, id, (void *)mutex,
                    (void *)__atomic_load_n(&oy_threads[i].caller, __ATOMIC_RELAXED));
                oy_stall_write(line);
            }
        }
        oy_allocator_snapshot(line, sizeof(line));
        oy_stall_write(line);
        oy_stall_batching = 0;
        oy_stall_write(oy_stall_batch);
    }
    return 0;
}
struct OyStallProgress *oy_stall_frame(unsigned enabled)
{
    __atomic_store_n(&oy_stall_capture_enabled, enabled != 0, __ATOMIC_RELAXED);
    if (!enabled) { __atomic_store_n(&oy_stall.enabled, 0, __ATOMIC_RELEASE); return NULL; }
    if (!oy_stall_attempted)
    {
        oy_stall_attempted = 1;
        __atomic_store_n(&oy_stall_game_thread, sceKernelGetThreadId(), __ATOMIC_RELAXED);
        oy_stall_started = 1;
        __atomic_store_n(&oy_stall_start_result, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&oy_stall_watch_entered, 1, __ATOMIC_RELEASE);
    }
    if (!oy_stall_started) { return NULL; }
    __atomic_store_n(&oy_stall.heartbeatUs, (unsigned)sceKernelGetProcessTimeWide(), __ATOMIC_RELAXED);
    __atomic_fetch_add(&oy_stall.frame, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&oy_stall.context, (OyStallFrame << 24) | 0xffffffu, __ATOMIC_RELAXED);
    __atomic_store_n(&oy_stall.enabled, 1, __ATOMIC_RELEASE);
    return &oy_stall;
}

// Single capture worker owns this native handle; never use libc FILE/descriptor translation.
static SceUID oy_binary_capture_fd = -1;
int oy_capture_open(void)
{
    if (oy_binary_capture_fd < 0)
        oy_binary_capture_fd = sceIoOpen("ux0:data/openyamm/incident-01.06.bin", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    return oy_binary_capture_fd < 0 ? -1 : 0;
}
int oy_capture_write(const void *data, unsigned size)
{
    // One owner writes frames, houses and compact independent heartbeat records.
    if (oy_binary_capture_fd < 0 || (size != 61952 && size != 66048 && size != 2048)) return -1;
    const char *p = data;
    while (size) {
        unsigned chunk = size > 16384 ? 16384 : size;
        int n = sceIoWrite(oy_binary_capture_fd, p, chunk);
        if (n <= 0) { sceIoClose(oy_binary_capture_fd); oy_binary_capture_fd = -1; return -1; }
        p += n; size -= n;
    }
    return 0;
}

static unsigned oy_capture_settings_id;
int oy_capture_settings(unsigned id, const char *text, unsigned size)
{
    oy_capture_settings_id = id;
    if (!text || size > 8191) return -1;
    char path[112];
    sceClibSnprintf(path,sizeof(path),"ux0:data/openyamm/capture-settings-01.06-%08x.ini",id);
    SceUID fd=sceIoOpen(path,SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,0666);
    if (fd<0) return -1;
    unsigned done=0;
    while (done<size) { int n=sceIoWrite(fd,text+done,size-done); if(n<=0) { sceIoClose(fd); return -1; } done+=n; }
    sceIoClose(fd); return 0;
}

// Capture-worker only; fixed storage. Kernel thread status is not PC sampling.
void oy_capture_snapshot(uint64_t *out, unsigned count)
{
    if (count < 192) return;
    memset(out, 0, 192 * sizeof(*out));
    const unsigned now = (unsigned)sceKernelGetProcessTimeWide();
    out[0]=now; out[1]=__atomic_load_n(&oy_stall.frame,__ATOMIC_RELAXED);
    out[2]=now-__atomic_load_n(&oy_stall.heartbeatUs,__ATOMIC_RELAXED);
    out[3]=__atomic_load_n(&oy_stall.context,__ATOMIC_RELAXED);
    out[4]=__atomic_load_n(&oy_stall.sector,__ATOMIC_RELAXED);
    out[5]=__atomic_load_n(&oy_stall.step,__ATOMIC_RELAXED);
    const unsigned seq=__atomic_load_n(&oy_stall.streamSequence,__ATOMIC_ACQUIRE);
    if (seq && !(seq&1))
    {
        for(unsigned i=0;i<20;++i) out[8+i]=__atomic_load_n(&oy_stall.streamTotals[i],__ATOMIC_RELAXED);
        out[6]=seq==__atomic_load_n(&oy_stall.streamSequence,__ATOMIC_ACQUIRE);
    }
    unsigned written=0;
    for(unsigned selectedRole=1;selectedRole<=8;++selectedRole)
    for(unsigned i=0;i<64;++i)
    {
        SceUID id=__atomic_load_n(&oy_threads[i].id,__ATOMIC_ACQUIRE);
        if(id<=0)continue;
        unsigned role=__atomic_load_n(&oy_threads[i].role,__ATOMIC_RELAXED);
        if(role!=selectedRole)continue;
        ++out[28];
        if(written>=10) { ++out[29]; continue; }
        uint64_t *v=out+32+written*16;
        SceKernelThreadInfo info={0};info.size=sizeof(info);
        const int result=sceKernelGetThreadInfo(id,&info);
        v[0]=(unsigned)id;v[1]=role;v[2]=(unsigned)result;
        v[3]=info.status;v[4]=info.waitType;v[5]=(unsigned)info.waitId;
        v[6]=info.currentPriority;v[7]=info.currentCpuId;v[8]=info.runClocks;
        v[9]=__atomic_load_n(&oy_threads[i].stage,__ATOMIC_ACQUIRE);
        v[10]=now-__atomic_load_n(&oy_threads[i].stageUs,__ATOMIC_RELAXED);
        v[11]=__atomic_load_n(&oy_threads[i].detail,__ATOMIC_RELAXED);
        v[12]=__atomic_load_n(&oy_threads[i].mutex,__ATOMIC_RELAXED);
        v[13]=__atomic_load_n(&oy_threads[i].caller,__ATOMIC_RELAXED);
        SceKernelMutexInfo mutex={0};mutex.size=sizeof(mutex);
        if(result==0 && info.waitId>0 && sceKernelGetMutexInfo(info.waitId,&mutex)==0)
            v[14]=(unsigned)mutex.currentOwnerId;
        if (!v[14] && oy_lock_diagnostics && v[12])
            v[14]=oy_native_lock_owner((uintptr_t)v[12]);
        v[15]=__atomic_load_n(&oy_threads[i].failure,__ATOMIC_RELAXED);
        ++written;
    }
    out[7]=written;
}
void oy_capture_heartbeat(void)
{
    static unsigned last;
    static uint64_t wire[256];
    if(!__atomic_load_n(&oy_stall_capture_enabled,__ATOMIC_RELAXED))return;
    const unsigned now=(unsigned)sceKernelGetProcessTimeWide();
    if(last && now-last<1000000)return;
    last=now;
    memset(wire,0,sizeof(wire));
    wire[0]=0x3138305759414d4dull;wire[1]=3;wire[2]=sizeof(wire);wire[11]=oy_capture_settings_id;
    // Keep the SDK's opaque work bytes raw: do not guess their owner/count layout.
    if (oy_lock_diagnostics && oy_original_heap_work) {
        const unsigned *words=(const unsigned *)oy_original_heap_work;
        for (unsigned i=0;i<4;++i) wire[3+i]=(uint64_t)__atomic_load_n(words+2*i,__ATOMIC_RELAXED)
            | ((uint64_t)__atomic_load_n(words+2*i+1,__ATOMIC_RELAXED)<<32);
    }
    wire[7]=__atomic_load_n(&oy_original_wait_total_us,__ATOMIC_RELAXED);
    wire[8]=__atomic_load_n(&oy_original_waits_ge_1ms,__ATOMIC_RELAXED);
    wire[9]=__atomic_load_n(&oy_path_yields,__ATOMIC_RELAXED);
    wire[10]=__atomic_load_n(&oy_path_max_gap_us,__ATOMIC_RELAXED);
    wire[12]=oy_lock_diagnostics;
    oy_capture_health(wire+16,23);
    for (unsigned i=0;i<10;++i) wire[40+i]=__atomic_load_n(&oy_activity[i],__ATOMIC_RELAXED);
    if (oy_lock_diagnostics) oy_native_lock_summary(wire+50);
    const unsigned heapOwner=__atomic_load_n(&oy_original_owner,__ATOMIC_ACQUIRE);
    wire[60]=heapOwner ? (unsigned)sceKernelGetProcessTimeWide()
        -__atomic_load_n(&oy_original_start,__ATOMIC_RELAXED) : 0;
    if (heapOwner!=__atomic_load_n(&oy_original_owner,__ATOMIC_ACQUIRE)) wire[60]=0;
    oy_capture_snapshot(wire+64,192);
    uint32_t checksum=2166136261u;
    const unsigned char *p=(const unsigned char *)wire;
    for(unsigned i=0;i<sizeof(wire);++i)checksum=(checksum^p[i])*16777619u;
    wire[63]=checksum;
    if(oy_binary_capture_fd<0)oy_capture_open();
    if(oy_binary_capture_fd>=0)oy_capture_write(wire,sizeof(wire));
    __atomic_fetch_add(&oy_stall_watch_polls,1,__ATOMIC_RELAXED);
}
