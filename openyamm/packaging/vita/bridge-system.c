#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>

void oy_sync_errno(void);
FILE *oy_stream(FILE *pStream);

void oy_arc4random_buf(void *pBuffer, size_t size)
{
    unsigned char *pCurrent = pBuffer;
    while (size != 0)
    {
        size_t chunk = size > 64 ? 64 : size;
        if (sceKernelGetRandomNumber(pCurrent, chunk) < 0)
        {
            abort();
        }
        pCurrent += chunk;
        size -= chunk;
    }
}

int oy_posix_memalign(void **ppResult, size_t alignment, size_t size)
{
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
    {
        return 22;
    }
    void *pAllocation = memalign(alignment, size);
    if (pAllocation == NULL && size != 0)
    {
        return 12;
    }
    *ppResult = pAllocation;
    return 0;
}

unsigned long oy_getauxval(unsigned long type)
{
    if (type == 16)
    {
        // ARM Cortex-A9: VFP, NEON, VFPv3 and VFPv3 D32. No VFPv4 or ARMv8 instructions.
        return (1u << 6) | (1u << 12) | (1u << 13) | (1u << 19);
    }
    if (type == 6) return 4096;
    if (type == 26) return 0;
    errno = ENOENT;
    oy_sync_errno();
    return 0;
}

int oy_sched_getaffinity(int pid, size_t size, void *pMask)
{
    if (pid != 0 || size < sizeof(uint32_t))
    {
        errno = EINVAL;
        oy_sync_errno();
        return -1;
    }
    int affinity = sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
    if (affinity < 0)
    {
        errno = EIO;
        oy_sync_errno();
        return -1;
    }
    uint32_t mask = ((uint32_t)affinity >> 16) & 7u;
    memset(pMask, 0, size);
    memcpy(pMask, &mask, sizeof(mask));
    return 0;
}

int oy___sched_cpucount(size_t size, const void *pMask)
{
    const unsigned char *pBytes = pMask;
    int count = 0;
    for (size_t index = 0; index < size; ++index)
    {
        count += __builtin_popcount(pBytes[index]);
    }
    return count;
}

long oy_sysconf(int name)
{
    switch (name)
    {
    case 0x06: return 100;
    case 0x0b: return sysconf(_SC_OPEN_MAX);
    case 0x27: return 4096;
    case 0x4c: return 16384;
    case 0x60: return 3;
    case 0x61:
    {
        uint32_t mask;
        if (oy_sched_getaffinity(0, sizeof(mask), &mask) < 0) return -1;
        return __builtin_popcount(mask);
    }
    default:
        errno = EINVAL;
        oy_sync_errno();
        return -1;
    }
}

long oy_pathconf(const char *pPath, int name)
{
    char native[1024];
    pPath = oy_native_path(pPath, native, sizeof(native));
    if (pPath == NULL) return -1;
    // Android uses 4/5 for NAME_MAX/PATH_MAX; newlib uses 3/4.
    if (name == 4 || name == 5)
    {
        // A -1 result with errno==0 means the limit is indeterminate, not a syscall error.
        errno = 0;
        long result = pathconf(pPath, name == 4 ? _PC_NAME_MAX : _PC_PATH_MAX);
        oy_sync_errno();
        return result;
    }
    errno = EINVAL;
    oy_sync_errno();
    return -1;
}

int oy_prctl(int operation, ...)
{
    (void)operation;
    errno = ENOSYS;
    oy_sync_errno();
    return -1;
}

int oy_fork(void)
{
    errno = ENOSYS;
    oy_sync_errno();
    return -1;
}

int oy_execvp(const char *pFile, char *const pArguments[])
{
    (void)pFile;
    (void)pArguments;
    errno = ENOSYS;
    oy_sync_errno();
    return -1;
}

int oy_waitpid(int pid, int *pStatus, int options)
{
    (void)pid;
    (void)pStatus;
    (void)options;
    errno = ECHILD;
    oy_sync_errno();
    return -1;
}

int oy___register_atfork(void (*pPrepare)(void), void (*pParent)(void), void (*pChild)(void), void *pDso)
{
    (void)pPrepare;
    (void)pParent;
    (void)pChild;
    (void)pDso;
    return 38;
}

void *oy_getpwuid(unsigned int uid)
{
    (void)uid;
    errno = ENOENT;
    oy_sync_errno();
    return NULL;
}

int oy_fsync(int descriptor)
{
    if (oy_asset_index(descriptor) >= 0) {
        struct OyAssetFd *p = oy_asset_grab(descriptor);
        if (!p) { oy_sync_errno(); return -1; }
        oy_asset_drop(p); return 0; // Immutable read-only handle has no pending writes.
    }
    int result = fsync(descriptor);
    oy_sync_errno();
    return result;
}

int oy_fseeko(FILE *pStream, int32_t offset, int origin)
{
    int result = fseek(oy_stream(pStream), offset, origin);
    oy_sync_errno();
    return result;
}

void oy_rewind(FILE *pStream)
{
    rewind(oy_stream(pStream));
    oy_sync_errno();
}

int32_t oy_ftello(FILE *pStream)
{
    long result = ftell(oy_stream(pStream));
    oy_sync_errno();
    return result;
}

int32_t oy_clock(void)
{
    return (int32_t)((int64_t)clock() * 1000000 / CLOCKS_PER_SEC);
}

int oy_fcntl(int descriptor, int command, ...)
{
    if (oy_asset_index(descriptor) >= 0) {
        struct OyAssetFd *p = oy_asset_grab(descriptor); if (!p) { oy_sync_errno(); return -1; }
        int result = 0;
        if (command == 1) result = __atomic_load_n(&p->flags, __ATOMIC_RELAXED);
        else if (command == 2) { va_list a; va_start(a, command); unsigned flags = va_arg(a, int); va_end(a); __atomic_store_n(&p->flags, flags & 1u, __ATOMIC_RELAXED); }
        else if (command == 3) result = 0; // Android F_GETFL: O_RDONLY, needed by PhysicsFS flush.
        else { errno = ENOTSUP; result = -1; }
        oy_asset_drop(p); oy_sync_errno(); return result;
    }
    int result;
    if (command == 1)
    {
        result = fcntl(descriptor, F_GETFD);
    }
    else if (command == 2)
    {
        va_list arguments;
        va_start(arguments, command);
        int flags = va_arg(arguments, int);
        va_end(arguments);
        result = fcntl(descriptor, F_SETFD, flags);
    }
    else
    {
        errno = ENOTSUP;
        result = -1;
    }
    oy_sync_errno();
    return result;
}

void oy_openlog(const char *pIdentity, int options, int facility)
{
    fprintf(stderr, "syslog opened: %s (%d, %d)\n", pIdentity != NULL ? pIdentity : "", options, facility);
}

void oy_closelog(void)
{
    fflush(stderr);
}

void oy_syslog(int priority, const char *pFormat, ...)
{
    fprintf(stderr, "syslog[%d] ", priority);
    va_list arguments;
    va_start(arguments, pFormat);
    vfprintf(stderr, pFormat, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}
