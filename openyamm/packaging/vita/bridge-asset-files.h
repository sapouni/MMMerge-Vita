#pragma once
#include <psp2/io/fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
// Immutable app assets and worker-owned read-only cache handles. Writable opens keep libc semantics.
#define OY_ASSET_FD_BASE 0x41000000
#define OY_ASSET_FD_COUNT 64
#define OY_ASSET_CLOSED 0x80000000u
struct OyAssetFd { int uid; unsigned refs, flags; struct stat metadata; };
static struct OyAssetFd oy_asset_fds[OY_ASSET_FD_COUNT];
static int oy_asset_index(int fd) {
    unsigned i = (unsigned)fd - OY_ASSET_FD_BASE;
    return i < OY_ASSET_FD_COUNT ? (int)i : -1;
}
static struct OyAssetFd *oy_asset_grab(int fd) {
    int i = oy_asset_index(fd);
    if (i < 0) { errno = EBADF; return NULL; }
    struct OyAssetFd *p = &oy_asset_fds[i];
    if (__atomic_load_n(&p->uid, __ATOMIC_ACQUIRE) <= 0) { errno = EBADF; return NULL; }
    unsigned n = __atomic_load_n(&p->refs, __ATOMIC_ACQUIRE);
    while (n && !(n & OY_ASSET_CLOSED)) {
        if (__atomic_compare_exchange_n(&p->refs, &n, n + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return p;
    }
    errno = EBADF; return NULL;
}
static int oy_asset_drop(struct OyAssetFd *p) {
    unsigned previous = __atomic_fetch_sub(&p->refs, 1, __ATOMIC_ACQ_REL);
    if ((previous & ~OY_ASSET_CLOSED) != 1) return 0;
    int result = sceIoClose(p->uid);
    __atomic_store_n(&p->uid, 0, __ATOMIC_RELEASE);
    if (result < 0) { errno = EIO; return -1; }
    return 0;
}
static int oy_asset_open(const char *path, int flags) {
    // Return -2 for ordinary libc fallback, including an exhausted fixed table.
    const int immutable = !strncmp(path, "app0:/assets/", 13)
        || !strncmp(path, "ux0:data/openyamm/sprite-cache-v3/", sizeof("ux0:data/openyamm/sprite-cache-v3/") - 1);
    if (!immutable || (flags & ~(0x8000 | 0x80000))) return -2;
    for (unsigned i = 0; i < OY_ASSET_FD_COUNT; ++i) {
        struct OyAssetFd *p = &oy_asset_fds[i]; int empty = 0;
        if (!__atomic_compare_exchange_n(&p->uid, &empty, -1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
        struct stat metadata;
        if (stat(path, &metadata) || !S_ISREG(metadata.st_mode)) { __atomic_store_n(&p->uid, 0, __ATOMIC_RELEASE); return -2; }
        int uid = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (uid < 0) { __atomic_store_n(&p->uid, 0, __ATOMIC_RELEASE); return -2; }
        p->metadata = metadata; p->flags = (flags & 0x80000) ? 1u : 0u;
        __atomic_store_n(&p->refs, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&p->uid, uid, __ATOMIC_RELEASE);
        return OY_ASSET_FD_BASE + i;
    }
    return -2;
}
static int oy_asset_close(int fd) {
    struct OyAssetFd *p = oy_asset_grab(fd);
    if (!p) return -1;
    unsigned previous = __atomic_fetch_or(&p->refs, OY_ASSET_CLOSED, __ATOMIC_ACQ_REL);
    if (previous & OY_ASSET_CLOSED) { oy_asset_drop(p); errno = EBADF; return -1; }
    oy_asset_drop(p); // release the open-handle reference
    return oy_asset_drop(p); // release this operation; in-flight reads retain the native UID
}
static ssize_t oy_asset_read(int fd, void *buffer, size_t size) {
    struct OyAssetFd *p = oy_asset_grab(fd); if (!p) return -1;
    int n = sceIoRead(p->uid, buffer, size > INT_MAX ? INT_MAX : size);
    oy_asset_drop(p); if (n < 0) { errno = EIO; return -1; } return n;
}
static int64_t oy_asset_seek(int fd, int64_t offset, int origin) {
    struct OyAssetFd *p = oy_asset_grab(fd); if (!p) return -1;
    if (origin < 0 || origin > 2) { oy_asset_drop(p); errno = EINVAL; return -1; }
    int64_t n = sceIoLseek(p->uid, offset, origin);
    oy_asset_drop(p); if (n < 0) { errno = EIO; return -1; } return n;
}
