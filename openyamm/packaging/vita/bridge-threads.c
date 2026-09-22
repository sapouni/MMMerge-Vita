#include "android-abi.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
void oy_stall_mutex_completed(unsigned begin, uintptr_t caller);

_Static_assert(sizeof(pthread_mutex_t) == 4, "Vita mutex handle size");
_Static_assert(sizeof(pthread_cond_t) == 4, "Vita condition handle size");

// VitaSDK owns lazy initialization after the Android sentinel has been translated atomically.
static void prepareMutex(_Atomic(uintptr_t) *pMutex)
{
    uintptr_t value = atomic_load(pMutex);
    uintptr_t replacement;
    switch (value)
    {
    case 0: replacement = (uintptr_t)PTHREAD_MUTEX_INITIALIZER; break;
    case 0x4000: replacement = (uintptr_t)PTHREAD_RECURSIVE_MUTEX_INITIALIZER; break;
    case 0x8000: replacement = (uintptr_t)PTHREAD_ERRORCHECK_MUTEX_INITIALIZER; break;
    default: return;
    }
    atomic_compare_exchange_strong(pMutex, &value, replacement);
}

static void prepareCondition(_Atomic(uintptr_t) *pCondition)
{
    uintptr_t value = 0;
    atomic_compare_exchange_strong(pCondition, &value, (uintptr_t)PTHREAD_COND_INITIALIZER);
}

int oy_pthread_mutex_init(pthread_mutex_t *pMutex, const uint32_t *pAttributes)
{
    pthread_mutexattr_t attributes;
    int result = pthread_mutexattr_init(&attributes);
    if (result != 0)
    {
        return result;
    }
    // Android stores the mutex type in the low four bits; process sharing is unsupported here.
    uint32_t flags = pAttributes != NULL ? *pAttributes : 0;
    if ((flags & ~3u) != 0 || (flags & 3u) > 2)
    {
        pthread_mutexattr_destroy(&attributes);
        return 22;
    }
    pthread_mutexattr_settype(&attributes, flags & 3u);
    result = pthread_mutex_init(pMutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    return result;
}

extern unsigned oy_stall_capture_enabled;
extern unsigned oy_lock_diagnostics;
void oy_stall_mutex_wait(uintptr_t mutex, uintptr_t caller);
int oy_pthread_mutex_lock(pthread_mutex_t *pMutex)
{
    prepareMutex((_Atomic(uintptr_t) *)pMutex);
    if (!oy_lock_diagnostics && !oy_stall_capture_enabled)
        return pthread_mutex_lock(pMutex);
    const int immediate = pthread_mutex_trylock(pMutex);
    if (immediate != EBUSY) return immediate;
    // Capture observes the same blocking call as normal mode; never change acquisition order.
    oy_stall_mutex_wait((uintptr_t)pMutex, (uintptr_t)__builtin_return_address(0));
    unsigned begin = (unsigned)sceKernelGetProcessTimeWide();
    int result = pthread_mutex_lock(pMutex);
    oy_stall_mutex_completed(begin, (uintptr_t)__builtin_return_address(0));
    oy_stall_mutex_wait(0, 0);
    return result;
}

int oy_pthread_mutex_unlock(pthread_mutex_t *pMutex)
{
    prepareMutex((_Atomic(uintptr_t) *)pMutex);
    return pthread_mutex_unlock(pMutex);
}

int oy_pthread_mutex_destroy(pthread_mutex_t *pMutex)
{
    prepareMutex((_Atomic(uintptr_t) *)pMutex);
    return pthread_mutex_destroy(pMutex);
}

int oy_pthread_mutexattr_init(uint32_t *pAttributes)
{
    *pAttributes = 0;
    return 0;
}

int oy_pthread_mutexattr_destroy(uint32_t *pAttributes)
{
    // Android's attribute is an inline flags word, not a VitaSDK-owned handle.
    // There is no allocation to free. Mark it invalid after its lifetime ends.
    *pAttributes = UINT32_MAX;
    return 0;
}

int oy_pthread_mutexattr_settype(uint32_t *pAttributes, int type)
{
    if (type < 0 || type > 2)
    {
        return 22;
    }
    *pAttributes = (*pAttributes & ~15u) | type;
    return 0;
}

int oy_pthread_cond_init(pthread_cond_t *pCondition, const uint32_t *pAttributes)
{
    if (pAttributes != NULL && *pAttributes != 0)
    {
        return 22;
    }
    return pthread_cond_init(pCondition, NULL);
}

int oy_pthread_cond_destroy(pthread_cond_t *pCondition)
{
    prepareCondition((_Atomic(uintptr_t) *)pCondition);
    return pthread_cond_destroy(pCondition);
}

int oy_pthread_cond_signal(pthread_cond_t *pCondition)
{
    prepareCondition((_Atomic(uintptr_t) *)pCondition);
    return pthread_cond_signal(pCondition);
}

int oy_pthread_cond_broadcast(pthread_cond_t *pCondition)
{
    prepareCondition((_Atomic(uintptr_t) *)pCondition);
    return pthread_cond_broadcast(pCondition);
}

int oy_pthread_cond_wait(pthread_cond_t *pCondition, pthread_mutex_t *pMutex)
{
    prepareCondition((_Atomic(uintptr_t) *)pCondition);
    prepareMutex((_Atomic(uintptr_t) *)pMutex);
    return pthread_cond_wait(pCondition, pMutex);
}

int oy_pthread_cond_timedwait(pthread_cond_t *pCondition, pthread_mutex_t *pMutex,
    const AndroidTimespec *pDeadline)
{
    prepareCondition((_Atomic(uintptr_t) *)pCondition);
    prepareMutex((_Atomic(uintptr_t) *)pMutex);
    struct timespec deadline = {pDeadline->seconds, pDeadline->nanoseconds};
    int result = pthread_cond_timedwait(pCondition, pMutex, &deadline);
    return result == ETIMEDOUT ? 110 : result;
}

int oy_pthread_once(_Atomic(int) *pControl, void (*pInitialize)(void))
{
    int expected = 0;
    if (atomic_compare_exchange_strong(pControl, &expected, 1))
    {
        pInitialize();
        atomic_store_explicit(pControl, 2, memory_order_release);
    }
    else
    {
        while (atomic_load_explicit(pControl, memory_order_acquire) != 2)
        {
            sceKernelDelayThread(100);
        }
    }
    return 0;
}

int oy_pthread_attr_init(AndroidThreadAttributes *pAttributes)
{
    memset(pAttributes, 0, sizeof(*pAttributes));
    pAttributes->stackSize = 1024 * 1024;
    pAttributes->guardSize = 4096;
    return 0;
}

int oy_pthread_attr_setstacksize(AndroidThreadAttributes *pAttributes, size_t size)
{
    if (size < 16384)
    {
        return 22;
    }
    pAttributes->stackSize = size;
    return 0;
}

void oy_stall_register_thread(const char *role);
void oy_stall_unregister_thread(void);
struct OyThreadStart { void *(*start)(void *); void *argument; };
static void *oy_registered_thread_start(void *opaque)
{
    const struct OyThreadStart start = *(struct OyThreadStart *)opaque;
    free(opaque);
    oy_stall_register_thread("guest");
    void *result = start.start(start.argument);
    oy_stall_unregister_thread();
    return result;
}
int oy_pthread_create(pthread_t *pThread, const AndroidThreadAttributes *pAttributes,
    void *(*pStart)(void *), void *pArgument)
{
    pthread_attr_t attributes;
    int result = pthread_attr_init(&attributes);
    if (result != 0)
    {
        return result;
    }
    if (pAttributes != NULL && (pAttributes->stackBase != 0 || pAttributes->policy != 0))
    {
        pthread_attr_destroy(&attributes);
        return 22;
    }
    result = pthread_attr_setstacksize(&attributes, pAttributes != NULL ? pAttributes->stackSize : 1024 * 1024);
    if (result == 0 && pAttributes != NULL && (pAttributes->flags & 1u))
    {
        result = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    }
    if (result == 0)
    {
        struct OyThreadStart *start = malloc(sizeof(*start));
        if (!start) { result = ENOMEM; }
        else
        {
            start->start = pStart; start->argument = pArgument;
            result = pthread_create(pThread, &attributes, oy_registered_thread_start, start);
            if (result) { free(start); }
        }
    }
    fprintf(stderr, "VITA PTHREAD_CREATE result=%d stack_bytes=%u start=%p thread=%u\n",
        result, (unsigned)(pAttributes ? pAttributes->stackSize : 1024 * 1024),
        (void *)pStart, result == 0 ? (unsigned)*pThread : 0);
    fflush(stderr);
    pthread_attr_destroy(&attributes);
    return result;
}

// Native libraries (newlib/SDL/vitaGL) can block below the Android pthread shim.
// Preserve SDK lock semantics and keep normal builds on the direct path.
#include "bridge-native-locks.h"
void oy_stall_mutex_wait_for(unsigned, uintptr_t, uintptr_t);
void oy_stall_mutex_completed_for(unsigned, unsigned, uintptr_t);
int __real_sceKernelLockLwMutex(SceKernelLwMutexWork *, int, unsigned int *);
int __wrap_sceKernelLockLwMutex(SceKernelLwMutexWork *work, int count, unsigned int *timeout)
{
    if (!oy_lock_diagnostics)
        return __real_sceKernelLockLwMutex(work, count, timeout);
    unsigned id = (unsigned)sceKernelGetThreadId();
    unsigned caller = (unsigned)__builtin_return_address(0);
    oy_stall_mutex_wait_for(id,(uintptr_t)work,caller);
    unsigned begin = (unsigned)sceKernelGetProcessTimeWide();
    int result = __real_sceKernelLockLwMutex(work,count,timeout);
    unsigned now = (unsigned)sceKernelGetProcessTimeWide();
    if (result >= 0) oy_native_acquired_at((uintptr_t)work,caller,begin,count,now,id);
    oy_stall_mutex_completed_for(id,now-begin,caller);
    oy_stall_mutex_wait_for(id,0,0);
    return result;
}

int __real_sceKernelUnlockLwMutex(SceKernelLwMutexWork *, int);
int __real_sceKernelTryLockLwMutex(SceKernelLwMutexWork *, int);
int __wrap_sceKernelTryLockLwMutex(SceKernelLwMutexWork *work, int count)
{
    if (!oy_lock_diagnostics) return __real_sceKernelTryLockLwMutex(work, count);
    const unsigned begin = (unsigned)sceKernelGetProcessTimeWide();
    const int result = __real_sceKernelTryLockLwMutex(work, count);
    if (result >= 0) oy_native_acquired((uintptr_t)work, (unsigned)__builtin_return_address(0), begin, count);
    return result;
}
int __wrap_sceKernelUnlockLwMutex(SceKernelLwMutexWork *work, int count)
{
    if (oy_lock_diagnostics) oy_native_releasing((uintptr_t)work, count);
    return __real_sceKernelUnlockLwMutex(work, count);
}
