#define _GNU_SOURCE
#include "android-abi.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Error translation and stream wrappers are supplied by the runtime bridge.
void oy_sync_errno(void);

static void convertStat(const struct stat *pSource, AndroidStat *pDestination)
{
    memset(pDestination, 0, sizeof(*pDestination));
    pDestination->device = pSource->st_dev;
    pDestination->legacyInode = pSource->st_ino;
    pDestination->inode = pSource->st_ino;
    pDestination->mode = pSource->st_mode;
    pDestination->links = pSource->st_nlink;
    pDestination->uid = pSource->st_uid;
    pDestination->gid = pSource->st_gid;
    pDestination->specialDevice = pSource->st_rdev;
    pDestination->size = pSource->st_size;
    pDestination->blockSize = pSource->st_blksize;
    pDestination->blocks = pSource->st_blocks;
    pDestination->accessTime.seconds = pSource->st_atime;
    pDestination->modifyTime.seconds = pSource->st_mtime;
    pDestination->changeTime.seconds = pSource->st_ctime;
}

int oy_stat(const char *pPath, AndroidStat *pResult)
{
    char native[1024];
    pPath = oy_native_path(pPath, native, sizeof(native));
    if (pPath == NULL) return -1;
    struct stat status;
    int result = stat(pPath, &status);
    if (result == 0)
    {
        convertStat(&status, pResult);
    }
    oy_sync_errno();
    return result;
}

int oy_lstat(const char *pPath, AndroidStat *pResult)
{
    char native[1024];
    pPath = oy_native_path(pPath, native, sizeof(native));
    if (pPath == NULL) return -1;
    struct stat status;
    int result = lstat(pPath, &status);
    if (result == 0)
    {
        convertStat(&status, pResult);
    }
    oy_sync_errno();
    return result;
}

int oy_fstat(int descriptor, AndroidStat *pResult)
{
    if (oy_asset_index(descriptor) >= 0) {
        struct OyAssetFd *p = oy_asset_grab(descriptor);
        if (!p) { oy_sync_errno(); return -1; }
        convertStat(&p->metadata, pResult); oy_asset_drop(p); return 0;
    }
    if (randomDescriptorIndex(descriptor) >= 0)
    {
        if (!randomDescriptorIsOpen(descriptor))
        {
            errno = EBADF;
            oy_sync_errno();
            return -1;
        }
        memset(pResult, 0, sizeof(*pResult));
        pResult->mode = 0020000 | 0444; // Android S_IFCHR and read permissions.
        pResult->links = 1;
        return 0;
    }
    struct stat status;
    int result = fstat(descriptor, &status);
    if (result == 0)
    {
        convertStat(&status, pResult);
    }
    oy_sync_errno();
    return result;
}

typedef struct AndroidDirectory
{
    DIR *pNative;
    AndroidDirent current;
    int64_t position;
} AndroidDirectory;

AndroidDirectory *oy_opendir(const char *pPath)
{
    char native[1024];
    pPath = oy_native_path(pPath, native, sizeof(native));
    if (pPath == NULL) return NULL;
    DIR *pNative = opendir(pPath);
    if (pNative == NULL)
    {
        oy_sync_errno();
        return NULL;
    }
    AndroidDirectory *pDirectory = calloc(1, sizeof(*pDirectory));
    if (pDirectory == NULL)
    {
        closedir(pNative);
        errno = ENOMEM;
        oy_sync_errno();
        return NULL;
    }
    pDirectory->pNative = pNative;
    return pDirectory;
}

AndroidDirent *oy_readdir(AndroidDirectory *pDirectory)
{
    // End-of-directory is distinguished from an error by errno, as required by libc++.
    errno = 0;
    struct dirent *pEntry = readdir(pDirectory->pNative);
    oy_sync_errno();
    if (pEntry == NULL)
    {
        return NULL;
    }
    AndroidDirent *pResult = &pDirectory->current;
    memset(pResult, 0, sizeof(*pResult));
    pResult->offset = ++pDirectory->position;
    pResult->recordLength = sizeof(*pResult);
    pResult->type = SCE_S_ISDIR(pEntry->d_stat.st_mode) ? 4 : 8;
    size_t length = strnlen(pEntry->d_name, sizeof(pResult->name));
    if (length == sizeof(pResult->name))
    {
        errno = ENAMETOOLONG;
        oy_sync_errno();
        return NULL;
    }
    memcpy(pResult->name, pEntry->d_name, length + 1);
    return pResult;
}

int oy_closedir(AndroidDirectory *pDirectory)
{
    int result = closedir(pDirectory->pNative);
    free(pDirectory);
    oy_sync_errno();
    return result;
}

int oy_open(const char *pPath, int flags, ...)
{
    if (pPath && strcmp(pPath, "/dev/urandom") == 0)
    {
        int result = openRandomDescriptor(flags);
        oy_sync_errno();
        return result;
    }
    char native[1024];
    pPath = oy_native_path(pPath, native, sizeof(native));
    if (pPath == NULL) return -1;
    mode_t mode = 0;
    if (flags & 0x40)
    {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, unsigned int);
        va_end(arguments);
    }
    // Only flags with equivalent semantics are accepted. LARGEFILE and CLOEXEC need no action on Vita.
    const int supported = 3 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800 | 0x8000 | 0x80000;
    if ((flags & ~supported) != 0)
    {
        errno = ENOTSUP;
        oy_sync_errno();
        return -1;
    }
    int direct = oy_asset_open(pPath, flags);
    if (direct >= 0) return direct;
    int nativeFlags = flags & 3;
    if (flags & 0x40) nativeFlags |= O_CREAT;
    if (flags & 0x80) nativeFlags |= O_EXCL;
    if (flags & 0x100) nativeFlags |= O_NOCTTY;
    if (flags & 0x200) nativeFlags |= O_TRUNC;
    if (flags & 0x400) nativeFlags |= O_APPEND;
    if (flags & 0x800) nativeFlags |= O_NONBLOCK;
    int result = open(pPath, nativeFlags, mode);
    oy_sync_errno();
    return result;
}

int64_t oy_lseek64(int descriptor, int64_t offset, int origin)
{
    if (oy_asset_index(descriptor) >= 0) { int64_t n = oy_asset_seek(descriptor, offset, origin); oy_sync_errno(); return n; }
    if (randomDescriptorIndex(descriptor) >= 0)
    {
        errno = randomDescriptorIsOpen(descriptor) ? ESPIPE : EBADF;
        oy_sync_errno();
        return -1;
    }
    if (offset > LONG_MAX || offset < LONG_MIN)
    {
        errno = EOVERFLOW;
        oy_sync_errno();
        return -1;
    }
    int64_t result = lseek(descriptor, (off_t)offset, origin);
    oy_sync_errno();
    return result;
}

int32_t oy_lseek(int descriptor, int32_t offset, int origin)
{
    return oy_lseek64(descriptor, offset, origin);
}

int oy_close(int descriptor)
{
    if (oy_asset_index(descriptor) >= 0) { int n = oy_asset_close(descriptor); oy_sync_errno(); return n; }
    int result = randomDescriptorIndex(descriptor) >= 0 ? closeRandomDescriptor(descriptor) : close(descriptor);
    oy_sync_errno();
    return result;
}

ssize_t oy_read(int descriptor, void *pBuffer, size_t size)
{
    if (oy_asset_index(descriptor) >= 0) { ssize_t n = oy_asset_read(descriptor, pBuffer, size); oy_sync_errno(); return n; }
    ssize_t result = randomDescriptorIndex(descriptor) >= 0
        ? readRandomDescriptor(descriptor, pBuffer, size) : read(descriptor, pBuffer, size);
    oy_sync_errno();
    return result;
}

int oy_fgetc(FILE *pStream)
{
    int result = fgetc(oy_stream(pStream));
    oy_sync_errno();
    return result;
}

char *oy_fgets(char *pBuffer, int size, FILE *pStream)
{
    char *result = fgets(pBuffer, size, oy_stream(pStream));
    oy_sync_errno();
    return result;
}
