#pragma once
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <reent.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

extern unsigned oy_stall_capture_enabled;
extern unsigned oy_lock_diagnostics;
void oy_stall_mutex_wait(uintptr_t, uintptr_t);
void __real___malloc_lock(struct _reent *);
void __real___malloc_unlock(struct _reent *);
// 00.85: change only the original newlib lightweight lock's wait queue.
// Called before main: native I/O and fixed storage only, no libc allocation/stdio.
static unsigned oy_heap_priority_requested = 1, oy_heap_priority_actual, oy_heap_creation_seen;
static int oy_heap_priority_error;
static SceKernelLwMutexWork *oy_original_heap_work;
int __real_sceKernelCreateLwMutex(SceKernelLwMutexWork *, const char *, unsigned int, int,
    const SceKernelLwMutexOptParam *);
static unsigned oy_heap_priority_setting(const char *data, unsigned size) {
    unsigned value = 1;
    const char key[] = "allocator_priority";
    for (unsigned p = 0; p < size;) {
        unsigned end = p; while (end < size && data[end] != '\n') ++end;
        while (p < end && (data[p] == ' ' || data[p] == '\t')) ++p;
        unsigned i = 0; while (i < sizeof(key)-1 && p+i < end && data[p+i] == key[i]) ++i;
        if (i == sizeof(key)-1) {
            unsigned q = p+i; while (q < end && (data[q]==' ' || data[q]=='\t')) ++q;
            if (q < end && data[q++] == '=') {
                while (q < end && (data[q]==' ' || data[q]=='\t')) ++q;
                if (q < end && (data[q]=='0' || data[q]=='1')) value = data[q]=='1';
            }
        }
        p = end+1;
    }
    return value;
}
int __wrap_sceKernelCreateLwMutex(SceKernelLwMutexWork *work, const char *name,
    unsigned int attr, int count, const SceKernelLwMutexOptParam *options) {
    const char expected[] = "malloc mutex";
    unsigned i = 0;
    if (name) while (i < sizeof(expected) && name[i] == expected[i]) ++i;
    if (i != sizeof(expected)) return __real_sceKernelCreateLwMutex(work,name,attr,count,options);
    char data[4096];
    int fd = sceIoOpen("ux0:data/openyamm/vita-performance.ini",SCE_O_RDONLY,0);
    if (fd >= 0) {
        int size = sceIoRead(fd,data,sizeof(data)); sceIoClose(fd);
        if (size > 0) oy_heap_priority_requested = oy_heap_priority_setting(data,(unsigned)size);
    }
    oy_heap_creation_seen = 1;
    unsigned selected = attr | (oy_heap_priority_requested ? SCE_KERNEL_ATTR_THREAD_PRIO : 0);
    int result = __real_sceKernelCreateLwMutex(work,name,selected,count,options);
    if (result < 0 && selected != attr) {
        oy_heap_priority_error = result;
        result = __real_sceKernelCreateLwMutex(work,name,attr,count,options);
        selected = attr;
    }
    oy_heap_priority_actual = result >= 0 && (selected & SCE_KERNEL_ATTR_THREAD_PRIO) != 0;
    if (result >= 0) oy_original_heap_work = work;
    return result; // Original newlib startup retains its own failure handling.
}
void oy_allocator_queue_state(unsigned *seen, unsigned *requested, unsigned *actual, int *error) {
    *seen=oy_heap_creation_seen; *requested=oy_heap_priority_requested;
    *actual=oy_heap_priority_actual; *error=oy_heap_priority_error;
}

static SceUID oy_allocator_mutex = -1;
static unsigned oy_original_owner, oy_original_depth, oy_original_caller, oy_original_acquires;
static unsigned oy_original_start, oy_original_max, oy_original_max_owner, oy_original_max_caller;
static unsigned oy_original_wait_max, oy_original_slow, oy_original_sbrk_at_start, oy_original_max_sbrks;
static unsigned oy_original_wait_total_us, oy_original_waits_ge_1ms;
static unsigned oy_sbrk_calls, oy_sbrk_grow, oy_sbrk_trim, oy_sbrk_us, oy_sbrk_max, oy_sbrk_failed;
// Eight allocator call sites; updates serialized by the EXISTING heap lock. No dynamic storage or logging here.
struct OyHeapSite { unsigned caller, count, us, max_us, slow, owner; };
static struct OyHeapSite oy_heap_sites[8];
void *__real__sbrk_r(struct _reent *, ptrdiff_t);
void *__wrap__sbrk_r(struct _reent *reent, ptrdiff_t increment) {
    if (!oy_lock_diagnostics)
        return __real__sbrk_r(reent, increment);
    unsigned start = (unsigned)sceKernelGetProcessTimeWide();
    void *result = __real__sbrk_r(reent, increment);
    unsigned elapsed = (unsigned)sceKernelGetProcessTimeWide() - start;
    __atomic_fetch_add(&oy_sbrk_calls, 1, __ATOMIC_RELAXED);
    if (increment > 0) __atomic_fetch_add(&oy_sbrk_grow, 1, __ATOMIC_RELAXED);
    if (increment < 0) __atomic_fetch_add(&oy_sbrk_trim, 1, __ATOMIC_RELAXED);
    if (result == (void *)-1) __atomic_fetch_add(&oy_sbrk_failed, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&oy_sbrk_us, elapsed, __ATOMIC_RELAXED);
    unsigned previous = __atomic_load_n(&oy_sbrk_max, __ATOMIC_RELAXED);
    while (previous < elapsed && !__atomic_compare_exchange_n(&oy_sbrk_max, &previous, elapsed, 0,
        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return result;
}
static unsigned oy_allocator_enabled, oy_allocator_depth;
static unsigned oy_allocator_hold_start, oy_allocator_owner_caller;
static unsigned oy_allocator_acquires, oy_allocator_wait_max, oy_allocator_hold_max;
static int oy_allocator_probe_result, oy_allocator_probe_priority = -1;

static void oy_allocator_max(unsigned *dest, unsigned n) {
    unsigned old = __atomic_load_n(dest, __ATOMIC_RELAXED);
    while (old < n && !__atomic_compare_exchange_n(dest, &old, n, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
static void oy_allocator_fatal(int code) {
    char text[160];
    sceClibSnprintf(text, sizeof(text), "ALLOCATOR LOCK FAILURE 00.71 code=0x%x; refusing unlocked heap access\n", code);
    SceUID fd = sceIoOpen("ux0:data/openyamm-boot.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) { sceIoWrite(fd, text, strlen(text)); sceIoClose(fd); }
    sceKernelExitProcess(2);
    for (;;) {} // Never return to an allocator without its lock.
}

// Called once, first in main, before starting SDL or any app worker. Pre-main allocations
// use the original lock. Never change lock families while worker allocations are active.
int oy_allocator_start(void) {
    int requested = 0;
    char settings[4096];
    SceUID fd = sceIoOpen("ux0:data/openyamm/vita-performance.ini", SCE_O_RDONLY, 0);
    if (fd >= 0) {
        int n = sceIoRead(fd, settings, sizeof(settings)-1); sceIoClose(fd);
        if (n > 0) {
            settings[n] = 0;
            for (char *p = settings; p && *p;) {
                char *next = strchr(p, '\n'); if (next) *next++ = 0;
                while (*p == ' ' || *p == '\t') ++p;
                if (!strncmp(p, "lock_diagnostics", 16)) {
                    char *q=p+16; while (*q==' ' || *q=='\t') ++q;
                    if (*q++=='=') { while (*q==' ' || *q=='\t') ++q; oy_lock_diagnostics=*q=='1'; }
                }
                if (!strncmp(p, "allocator_sync", 14)) {
                    p += 14; while (*p == ' ' || *p == '\t') ++p;
                    if (*p++ == '=') { while (*p == ' ' || *p == '\t') ++p; requested = *p == '1'; }
                }
                p = next;
            }
        }
    }
    if (!requested) return 0;
    SceKernelMutexOptParam options = {0}; options.size = sizeof(options); options.ceilingPriority = 128;
    int mutex = sceKernelCreateMutex("oy_allocator_ceiling", SCE_KERNEL_MUTEX_ATTR_RECURSIVE |
        SCE_KERNEL_MUTEX_ATTR_CEILING | SCE_KERNEL_ATTR_THREAD_PRIO, 0, &options);
    if (mutex < 0) return oy_allocator_probe_result = mutex;
    unsigned timeout = 100000; int held = 0;
    int rc = sceKernelLockMutex(mutex, 1, &timeout);
    if (rc >= 0) {
        held = 1; timeout = 100000; rc = sceKernelLockMutex(mutex, 1, &timeout);
        if (rc >= 0) {
            held = 2;
            SceKernelThreadInfo info = {0}; info.size = sizeof(info);
            rc = sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
            if (rc >= 0) {
                oy_allocator_probe_priority = info.currentPriority;
                if (info.currentPriority > 128) rc = -1; // Ceiling must actually take effect.
            }
        }
    }
    if (held) {
        int released = sceKernelUnlockMutex(mutex, held);
        if (released < 0) oy_allocator_fatal(released);
    }
    if (rc < 0) { sceKernelDeleteMutex(mutex); return oy_allocator_probe_result = rc; }
    oy_allocator_mutex = mutex;
    __atomic_store_n(&oy_allocator_enabled, 1, __ATOMIC_RELEASE);
    return oy_allocator_probe_result = 1;
}

void __wrap___malloc_lock(struct _reent *reent) {
    if (!__atomic_load_n(&oy_allocator_enabled, __ATOMIC_ACQUIRE)) {
        const unsigned capture = oy_lock_diagnostics;
        const unsigned begin = capture ? (unsigned)sceKernelGetProcessTimeWide() : 0;
        __real___malloc_lock(reent);
        // Protected by the existing recursive heap lock; scalar publication only.
        // No allocation, timer, owner query or priority change in normal mode.
        if (capture) {
            const unsigned now = (unsigned)sceKernelGetProcessTimeWide();
            oy_allocator_max(&oy_original_wait_max, now - begin);
            __atomic_fetch_add(&oy_original_wait_total_us, now - begin, __ATOMIC_RELAXED);
            if (now - begin >= 1000) __atomic_fetch_add(&oy_original_waits_ge_1ms, 1, __ATOMIC_RELAXED);
            if (oy_original_depth++ == 0) {
                __atomic_store_n(&oy_original_start, now, __ATOMIC_RELAXED);
                oy_original_sbrk_at_start = __atomic_load_n(&oy_sbrk_calls, __ATOMIC_RELAXED);
                __atomic_store_n(&oy_original_caller, (unsigned)__builtin_return_address(0), __ATOMIC_RELAXED);
                __atomic_store_n(&oy_original_owner, (unsigned)sceKernelGetThreadId(), __ATOMIC_RELEASE);
            }
            __atomic_fetch_add(&oy_original_acquires, 1, __ATOMIC_RELAXED);
        }
        return;
    }
    const unsigned capture = oy_lock_diagnostics;
    const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    unsigned begin = 0;
    if (capture) {
        begin = (unsigned)sceKernelGetProcessTimeWide();
        oy_stall_mutex_wait((uintptr_t)&oy_allocator_mutex, caller);
    }
    // EXACTLY the same kernel acquisition in normal and diagnostic modes, without try-lock.
    int rc = sceKernelLockMutex(oy_allocator_mutex, 1, NULL);
    if (rc < 0) oy_allocator_fatal(rc);
    unsigned now = capture ? (unsigned)sceKernelGetProcessTimeWide() : 0;
    if (capture) {
        oy_stall_mutex_wait(0, 0);
        oy_allocator_max(&oy_allocator_wait_max, now - begin);
        __atomic_fetch_add(&oy_allocator_acquires, 1, __ATOMIC_RELAXED);
    }
    if (oy_allocator_depth++ == 0) {
        __atomic_store_n(&oy_allocator_owner_caller, (unsigned)caller, __ATOMIC_RELAXED);
        __atomic_store_n(&oy_allocator_hold_start, now, __ATOMIC_RELEASE);
    }
}
void __wrap___malloc_unlock(struct _reent *reent) {
    if (!__atomic_load_n(&oy_allocator_enabled, __ATOMIC_ACQUIRE)) {
        if (oy_original_depth && --oy_original_depth == 0) {
            const unsigned elapsed = (unsigned)sceKernelGetProcessTimeWide()
                - __atomic_load_n(&oy_original_start, __ATOMIC_RELAXED);
            const unsigned owner = __atomic_load_n(&oy_original_owner, __ATOMIC_RELAXED);
            const unsigned caller = __atomic_load_n(&oy_original_caller, __ATOMIC_RELAXED);
            if (elapsed > __atomic_load_n(&oy_original_max, __ATOMIC_RELAXED)) {
                __atomic_store_n(&oy_original_max_owner, owner, __ATOMIC_RELAXED);
                __atomic_store_n(&oy_original_max_caller, caller, __ATOMIC_RELAXED);
                __atomic_store_n(&oy_original_max_sbrks,
                    __atomic_load_n(&oy_sbrk_calls, __ATOMIC_RELAXED) - oy_original_sbrk_at_start, __ATOMIC_RELAXED);
                __atomic_store_n(&oy_original_max, elapsed, __ATOMIC_RELEASE);
            }
            if (elapsed >= 1000) __atomic_fetch_add(&oy_original_slow, 1, __ATOMIC_RELAXED);
            for (unsigned i = 0; i < 8; ++i) {
                struct OyHeapSite *site = &oy_heap_sites[i];
                unsigned prior = __atomic_load_n(&site->caller, __ATOMIC_RELAXED);
                if (prior && prior != caller) continue;
                __atomic_store_n(&site->caller, caller, __ATOMIC_RELAXED);
                __atomic_fetch_add(&site->count, 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&site->us, elapsed, __ATOMIC_RELAXED);
                if (elapsed >= 1000) __atomic_fetch_add(&site->slow, 1, __ATOMIC_RELAXED);
                if (elapsed > __atomic_load_n(&site->max_us, __ATOMIC_RELAXED)) {
                    __atomic_store_n(&site->owner, owner, __ATOMIC_RELAXED);
                    __atomic_store_n(&site->max_us, elapsed, __ATOMIC_RELEASE);
                }
                break;
            }
            __atomic_store_n(&oy_original_owner, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&oy_original_caller, 0, __ATOMIC_RELAXED);
        }
        __real___malloc_unlock(reent); return;
    }
    if (!oy_allocator_depth) oy_allocator_fatal(-2);
    if (--oy_allocator_depth == 0) {
        unsigned begin = __atomic_load_n(&oy_allocator_hold_start, __ATOMIC_RELAXED);
        if (begin) oy_allocator_max(&oy_allocator_hold_max, (unsigned)sceKernelGetProcessTimeWide() - begin);
        __atomic_store_n(&oy_allocator_hold_start, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&oy_allocator_owner_caller, 0, __ATOMIC_RELAXED);
    }
    int rc = sceKernelUnlockMutex(oy_allocator_mutex, 1);
    if (rc < 0) oy_allocator_fatal(rc);
}
void oy_allocator_snapshot(char *line, unsigned capacity) {
    if (!__atomic_load_n(&oy_allocator_enabled, __ATOMIC_ACQUIRE)) {
        unsigned owner = __atomic_load_n(&oy_original_owner, __ATOMIC_ACQUIRE);
        unsigned age = owner ? (unsigned)sceKernelGetProcessTimeWide()
            - __atomic_load_n(&oy_original_start, __ATOMIC_RELAXED) : 0;
        if (owner != __atomic_load_n(&oy_original_owner, __ATOMIC_ACQUIRE)) { owner = age = 0; }
        sceClibSnprintf(line, capacity,
            "ALLOC_LOCK enabled=0 original_owner=%u original_caller=%x captured_acquires=%u hold_us=%u max_hold_us=%u max_owner=%u max_caller=%x max_hold_sbrks=%u max_wait_us=%u holds_ge_1ms=%u sbrk_calls=%u grow=%u trim=%u sbrk_us=%u sbrk_max_us=%u sbrk_fail=%u scheduling_unchanged=1\n",
            owner,
            __atomic_load_n(&oy_original_caller, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_original_acquires, __ATOMIC_RELAXED), age,
            __atomic_load_n(&oy_original_max, __ATOMIC_ACQUIRE),
            __atomic_load_n(&oy_original_max_owner, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_original_max_caller, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_original_max_sbrks, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_original_wait_max, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_original_slow, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_sbrk_calls, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_sbrk_grow, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_sbrk_trim, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_sbrk_us, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_sbrk_max, __ATOMIC_RELAXED),
            __atomic_load_n(&oy_sbrk_failed, __ATOMIC_RELAXED));
        return;
    }
    SceKernelMutexInfo info = {0}; info.size = sizeof(info);
    int rc = oy_allocator_mutex >= 0 ? sceKernelGetMutexInfo(oy_allocator_mutex, &info) : -1;
    unsigned begin = __atomic_load_n(&oy_allocator_hold_start, __ATOMIC_ACQUIRE);
    unsigned age = begin ? (unsigned)sceKernelGetProcessTimeWide() - begin : 0;
    sceClibSnprintf(line, capacity,
        "ALLOC_LOCK enabled=%u probe=%x probe_priority=%d query=%x id=%d owner=%d depth=%d waiters=%d caller=%x hold_age_us=%u max_wait_us=%u max_hold_us=%u captured_acquires=%u\n",
        __atomic_load_n(&oy_allocator_enabled, __ATOMIC_ACQUIRE), oy_allocator_probe_result, oy_allocator_probe_priority,
        rc, oy_allocator_mutex, info.currentOwnerId, info.currentCount, info.numWaitThreads,
        __atomic_load_n(&oy_allocator_owner_caller, __ATOMIC_RELAXED), age,
        __atomic_load_n(&oy_allocator_wait_max, __ATOMIC_RELAXED), __atomic_load_n(&oy_allocator_hold_max, __ATOMIC_RELAXED),
        __atomic_load_n(&oy_allocator_acquires, __ATOMIC_RELAXED));
}
