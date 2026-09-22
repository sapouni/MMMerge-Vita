#include <malloc.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>

#if OY_PROFILE_ENABLED
void oy_log_memory_snapshot(const char *stage)
{
    extern unsigned int _newlib_heap_size_user;
    int savedErrno = errno;
    uint64_t begin = sceKernelGetProcessTimeWide();
    struct mallinfo info = mallinfo();
    SceKernelFreeMemorySizeInfo available = {sizeof(available), 0, 0, 0};
    int query = sceKernelGetFreeMemorySize(&available);
    uint64_t elapsed = sceKernelGetProcessTimeWide() - begin;
    char message[384];
    // Avoid scanning vitaGL's uncached graphics heaps just to print a progress marker.
    int length = sceClibSnprintf(message, sizeof(message),
        "VITA MEMORY: stage=%s heap_used=%d heap_free=%d heap_committed=%d heap_capacity=%u "
        "kernel_query=%x kernel_user=%d kernel_cdram=%d kernel_phycont=%d sample_us=%llu\n",
        stage, info.uordblks, info.fordblks, info.arena, _newlib_heap_size_user, query, available.size_user, available.size_cdram, available.size_phycont,
        (unsigned long long)elapsed);
    if (length > 0 && length < (int)sizeof(message) && !oy_profile_capture_write(stderr,message,length))
        write(fileno(stderr), message, length);
    errno = savedErrno;
}

#endif

static void logAllocationFailure(const char *operation, size_t bytes, void *caller)
{
    int savedErrno = errno;
    struct mallinfo info = mallinfo();
    SceKernelFreeMemorySizeInfo available = {sizeof(available), 0, 0, 0};
    int query = sceKernelGetFreeMemorySize(&available);
    char message[384];
    int length = sceClibSnprintf(message, sizeof(message),
        "VITA ALLOCATION FAILED: %s bytes=%u caller=%p heap_used=%d heap_free=%d kernel_query=%x user=%d free_chunks=%d top_releasable=%d arena=%d\n",
        operation, (unsigned)bytes, caller, info.uordblks, info.fordblks, query, available.size_user, info.ordblks, info.keepcost, info.arena);
    // Use stderr's existing file offset, without allocating or reopening the log.
    if (length > 0 && length < (int)sizeof(message))
    {
#if OY_PROFILE_ENABLED
        if (!oy_profile_capture_write(stderr,message,length))
#endif
            write(fileno(stderr), message, length);
    }
    errno = savedErrno;
}


void *oy_malloc(size_t bytes)
{
#if OY_PROFILE_ENABLED && !OY_PROFILE_AGGREGATE
    uint64_t begin=oy_profile_now();
#endif
    void *result = malloc(bytes);
#if OY_PROFILE_ENABLED && !OY_PROFILE_AGGREGATE
    oy_profile_guest_alloc(bytes,result,begin);
#endif
    if (!result && bytes) logAllocationFailure("malloc", bytes, __builtin_return_address(0));
    return result;
}

void *oy_calloc(size_t count, size_t bytes)
{
#if OY_PROFILE_ENABLED && !OY_PROFILE_AGGREGATE
    uint64_t begin=oy_profile_now();
#endif
    void *result = calloc(count, bytes);
#if OY_PROFILE_ENABLED && !OY_PROFILE_AGGREGATE
    oy_profile_guest_alloc(count*bytes,result,begin);
#endif
    if (!result && count && bytes) logAllocationFailure("calloc", count * bytes, __builtin_return_address(0));
    return result;
}

void *oy_realloc(void *pointer, size_t bytes)
{
#if OY_PROFILE_ENABLED && !OY_PROFILE_AGGREGATE
    uint64_t begin=oy_profile_now();
#endif
    void *result = realloc(pointer, bytes);
#if OY_PROFILE_ENABLED && !OY_PROFILE_AGGREGATE
    oy_profile_guest_alloc(bytes,result,begin);
#endif
    if (!result && bytes) logAllocationFailure("realloc", bytes, __builtin_return_address(0));
    return result;
}

void oy_free(void *pointer)
{
    free(pointer);
}
