#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void oy_sync_errno(void);

char *oy_getenv(const char *pName)
{
    // Android libraries need POSIX paths within the guest filesystem namespace.
    static char guestHome[] = "/data";
    static char guestTemporary[] = "/tmp";
    if (strcmp(pName, "HOME") == 0) return guestHome;
    if (strcmp(pName, "TMPDIR") == 0) return guestTemporary;
    return getenv(pName);
}

const char *oy_native_path(const char *pPath, char *pBuffer, size_t capacity)
{
    if (pPath == NULL)
    {
        errno = EINVAL;
        oy_sync_errno();
        return NULL;
    }
    const char *pPrefix = NULL;
    size_t prefixLength = 0;
    if (strncmp(pPath, "/app", 4) == 0 && (pPath[4] == '/' || pPath[4] == 0))
    {
        pPrefix = "app0:";
        prefixLength = 4;
    }
    else if (strncmp(pPath, "/data", 5) == 0 && (pPath[5] == '/' || pPath[5] == 0))
    {
        pPrefix = "ux0:data/openyamm";
        prefixLength = 5;
    }
    else if (strncmp(pPath, "/tmp", 4) == 0 && (pPath[4] == '/' || pPath[4] == 0))
    {
        pPrefix = "ux0:data/openyamm/tmp";
        prefixLength = 4;
    }
    if (pPrefix == NULL)
    {
        return pPath;
    }
    int length = snprintf(pBuffer, capacity, "%s%s", pPrefix, pPath + prefixLength);
    if (length < 0 || (size_t)length >= capacity)
    {
        errno = ENAMETOOLONG;
        oy_sync_errno();
        return NULL;
    }
    return pBuffer;
}

static char *virtualPath(const char *pNative, char *pOutput, size_t capacity)
{
    const char *pPrefix;
    size_t skipped;
    if (strncmp(pNative, "app0:", 5) == 0)
    {
        pPrefix = "/app";
        skipped = 5;
    }
    else if (strncmp(pNative, "ux0:data/openyamm", 17) == 0)
    {
        pPrefix = "/data";
        skipped = 17;
    }
    else if (strncmp(pNative, "ux0:/data/openyamm", 18) == 0)
    {
        pPrefix = "/data";
        skipped = 18;
    }
    else
    {
        errno = EINVAL;
        oy_sync_errno();
        return NULL;
    }
    size_t needed = strlen(pPrefix) + strlen(pNative + skipped) + 1;
    if (pOutput == NULL)
    {
        capacity = needed;
        pOutput = malloc(capacity);
        if (pOutput == NULL)
        {
            oy_sync_errno();
            return NULL;
        }
    }
    if (needed > capacity)
    {
        errno = ERANGE;
        oy_sync_errno();
        return NULL;
    }
    snprintf(pOutput, capacity, "%s%s", pPrefix, pNative + skipped);
    return pOutput;
}

char *oy_getcwd(char *pBuffer, size_t capacity)
{
    char native[1024];
    if (getcwd(native, sizeof(native)) == NULL)
    {
        oy_sync_errno();
        return NULL;
    }
    return virtualPath(native, pBuffer, capacity);
}

char *oy_realpath(const char *pPath, char *pOutput)
{
    char native[1024];
    const char *pTranslated = oy_native_path(pPath, native, sizeof(native));
    if (pTranslated == NULL) return NULL;
    char *pResolved = realpath(pTranslated, NULL);
    if (pResolved == NULL)
    {
        oy_sync_errno();
        return NULL;
    }
    char *pResult = virtualPath(pResolved, pOutput, 1024);
    free(pResolved);
    return pResult;
}

const char *oy_SDL_GetBasePath(void)
{
    return "/app/";
}

int oy_mkstemp(char *pTemplate)
{
    char storage[1024];
    const char *pNative = oy_native_path(pTemplate, storage, sizeof(storage));
    if (pNative == NULL) return -1;
    if (pNative != storage)
    {
        if (strlen(pNative) >= sizeof(storage))
        {
            errno = ENAMETOOLONG;
            oy_sync_errno();
            return -1;
        }
        strcpy(storage, pNative);
    }
    int result = mkstemp(storage);
    oy_sync_errno();
    if (result >= 0)
    {
        size_t length = strlen(pTemplate);
        memcpy(pTemplate + length - 6, storage + strlen(storage) - 6, 6);
    }
    return result;
}
