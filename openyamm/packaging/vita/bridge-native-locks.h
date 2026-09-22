#pragma once
// Diagnostic-only fixed table. No allocations, stdio or extra lock acquisition.
struct OyNativeLock {
    uintptr_t work;
    unsigned owner, caller, depth, start, count, maxHold, maxOwner, maxCaller, maxWait, slow, sequence;
};
#define OY_NATIVE_LOCK_SLOTS 4096u
static struct OyNativeLock oy_native_locks[OY_NATIVE_LOCK_SLOTS];
static unsigned oy_native_overflow;
static struct OyNativeLock *oy_native_lock(uintptr_t work, int create)
{
    unsigned hash = ((unsigned)(work >> 4) * 2654435761u) >> 20;
    for (unsigned n = 0; n < OY_NATIVE_LOCK_SLOTS; ++n) {
        struct OyNativeLock *s = &oy_native_locks[(hash+n)&(OY_NATIVE_LOCK_SLOTS-1)];
        uintptr_t key = __atomic_load_n(&s->work, __ATOMIC_ACQUIRE);
        if (key == work) return s;
        if (!key && !create) return NULL; // No deletions: empty terminates a lookup.
        if (!key && create && __atomic_compare_exchange_n(&s->work, &key, work, 0,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return s;
    }
    if (create) __atomic_fetch_add(&oy_native_overflow, 1, __ATOMIC_RELAXED);
    return NULL;
}
static void oy_native_acquired_at(uintptr_t work, unsigned caller, unsigned begin, unsigned count, unsigned now, unsigned id)
{
    struct OyNativeLock *s = oy_native_lock(work, 1);
    if (!s) return;
    if (now-begin > __atomic_load_n(&s->maxWait, __ATOMIC_RELAXED))
        __atomic_store_n(&s->maxWait, now-begin, __ATOMIC_RELAXED);
    if (!s->depth) {
        __atomic_fetch_add(&s->sequence, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&s->caller, caller, __ATOMIC_RELAXED);
        __atomic_store_n(&s->start, now, __ATOMIC_RELAXED);
        __atomic_store_n(&s->owner, id, __ATOMIC_RELEASE);
        __atomic_fetch_add(&s->sequence, 1, __ATOMIC_RELEASE);
    }
    s->depth += count;
    __atomic_fetch_add(&s->count, 1, __ATOMIC_RELAXED);
}
static void oy_native_acquired(uintptr_t work, unsigned caller, unsigned begin, unsigned count)
{ oy_native_acquired_at(work,caller,begin,count,(unsigned)sceKernelGetProcessTimeWide(),(unsigned)sceKernelGetThreadId()); }
static void oy_native_releasing(uintptr_t work, unsigned count)
{
    struct OyNativeLock *s = oy_native_lock(work, 0);
    if (!s || !s->depth || s->owner != (unsigned)sceKernelGetThreadId()) return;
    if (count > s->depth) return; // Leave invalid unlock behavior to the kernel.
    s->depth -= count;
    if (s->depth) return;
    __atomic_fetch_add(&s->sequence, 1, __ATOMIC_ACQ_REL);
    unsigned elapsed = (unsigned)sceKernelGetProcessTimeWide() - s->start;
    if (elapsed > __atomic_load_n(&s->maxHold, __ATOMIC_RELAXED)) {
        __atomic_store_n(&s->maxOwner, s->owner, __ATOMIC_RELAXED);
        __atomic_store_n(&s->maxCaller, s->caller, __ATOMIC_RELAXED);
        __atomic_store_n(&s->maxHold, elapsed, __ATOMIC_RELEASE);
    }
    if (elapsed >= 1000) __atomic_fetch_add(&s->slow, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s->owner, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&s->sequence, 1, __ATOMIC_RELEASE);
}
unsigned oy_native_lock_owner(uintptr_t work)
{
    struct OyNativeLock *s = oy_native_lock(work, 0);
    return s ? __atomic_load_n(&s->owner, __ATOMIC_ACQUIRE) : 0;
}
void oy_native_lock_summary(uint64_t *out)
{
    memset(out, 0, 10*sizeof(*out));
    for (unsigned i=0; i<OY_NATIVE_LOCK_SLOTS; ++i) {
        struct OyNativeLock *s = &oy_native_locks[i];
        uintptr_t key = __atomic_load_n(&s->work, __ATOMIC_ACQUIRE);
        if (!key) continue;
        const unsigned sequence = __atomic_load_n(&s->sequence, __ATOMIC_ACQUIRE);
        if (sequence & 1u) continue;
        const unsigned maximum = __atomic_load_n(&s->maxHold, __ATOMIC_ACQUIRE);
        const unsigned maxOwner = __atomic_load_n(&s->maxOwner, __ATOMIC_RELAXED);
        const unsigned maxCaller = __atomic_load_n(&s->maxCaller, __ATOMIC_RELAXED);
        const unsigned wait = __atomic_load_n(&s->maxWait, __ATOMIC_RELAXED);
        const unsigned slow = __atomic_load_n(&s->slow, __ATOMIC_RELAXED);
        const unsigned owner = __atomic_load_n(&s->owner, __ATOMIC_ACQUIRE);
        const unsigned start = __atomic_load_n(&s->start, __ATOMIC_RELAXED);
        // Read the clock AFTER the start. The old scan-wide clock could predate a
        // new acquisition and wrap a few microseconds into a bogus ~4295s hold.
        const unsigned age = owner ? (unsigned)sceKernelGetProcessTimeWide() - start : 0;
        if (sequence != __atomic_load_n(&s->sequence, __ATOMIC_ACQUIRE)) continue;
        if (maximum > out[0]) { out[0]=maximum;out[1]=key;out[2]=maxOwner;out[3]=maxCaller; }
        if (wait > out[4]) out[4]=wait;
        out[5] += slow;
        if (age > out[6]) { out[6]=age;out[7]=key;out[8]=owner; }
    }
    out[9]=__atomic_load_n(&oy_native_overflow, __ATOMIC_RELAXED);
}
