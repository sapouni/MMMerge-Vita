#include <stdarg.h>
#if OY_PROFILE_ENABLED
int oy_profile_capture_write(FILE *, const void *, size_t);
int oy_profile_capture_vprintf(FILE *, const char *, va_list, int *);
int oy_profile_fprintf(FILE *, const char *, ...);
#else
#define oy_profile_capture_write(...) 0
#define oy_profile_capture_vprintf(...) 0
#endif
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct ThreadDestructor
{
    void (*pFunction)(void *);
    void *pObject;
    struct ThreadDestructor *pNext;
} ThreadDestructor;

typedef struct RuntimeThread
{
    int androidErrno;
    unsigned char mbstate[4];
    uintptr_t eglState[3];
    ThreadDestructor *pDestructors;
} RuntimeThread;

static pthread_key_t runtimeKey;
static pthread_once_t runtimeOnce = PTHREAD_ONCE_INIT;

static void destroyRuntimeThread(void *pArgument)
{
    RuntimeThread *pThread = pArgument;
    pthread_setspecific(runtimeKey, pThread);
    while (pThread->pDestructors != NULL)
    {
        ThreadDestructor *pDestructor = pThread->pDestructors;
        pThread->pDestructors = pDestructor->pNext;
        void (*pFunction)(void *) = pDestructor->pFunction;
        void *pObject = pDestructor->pObject;
        free(pDestructor);
        pFunction(pObject);
    }
    pthread_setspecific(runtimeKey, NULL);
    free(pThread);
}

static void initializeRuntimeKey(void)
{
    if (pthread_key_create(&runtimeKey, destroyRuntimeThread) != 0)
    {
        abort();
    }
}

static RuntimeThread *runtimeThread(void)
{
    pthread_once(&runtimeOnce, initializeRuntimeKey);
    RuntimeThread *pThread = pthread_getspecific(runtimeKey);
    if (pThread == NULL)
    {
        pThread = calloc(1, sizeof(*pThread));
        if (pThread == NULL || pthread_setspecific(runtimeKey, pThread) != 0)
        {
            abort();
        }
    }
    return pThread;
}

int oy_translate_errno(int error)
{
    // The first 34 POSIX error numbers agree. Newlib's extended range differs from Linux/Bionic.
    if (error <= 34)
    {
        return error;
    }
    switch (error)
    {
    case EDEADLK: return 35;
    case ENAMETOOLONG: return 36;
    case ENOLCK: return 37;
    case ENOSYS: return 38;
    case ENOTEMPTY: return 39;
    case ELOOP: return 40;
    case ENOMSG: return 42;
    case EIDRM: return 43;
    case EOVERFLOW: return 75;
    case EILSEQ: return 84;
    case ENOTSOCK: return 88;
    case ENOTSUP: return 95;
    case ETIMEDOUT: return 110;
    case ECONNREFUSED: return 111;
    case ECANCELED: return 125;
    case EOWNERDEAD: return 130;
    case ENOTRECOVERABLE: return 131;
    default: return 5;
    }
}

void oy_sync_errno(void)
{
    int error = errno;
    runtimeThread()->androidErrno = oy_translate_errno(error);
}

int *oy___errno(void)
{
    return &runtimeThread()->androidErrno;
}

unsigned char *oy_default_mbstate(void)
{
    return runtimeThread()->mbstate;
}

uintptr_t *oy_egl_state(void)
{
    return runtimeThread()->eglState;
}

int oy___cxa_thread_atexit_impl(void (*pFunction)(void *), void *pObject, void *pDso)
{
    (void)pDso; // The guest library stays loaded until the process exits.
    RuntimeThread *pThread = runtimeThread();
    ThreadDestructor *pDestructor = malloc(sizeof(*pDestructor));
    if (pDestructor == NULL)
    {
        return -1;
    }
    *pDestructor = (ThreadDestructor){pFunction, pObject, pThread->pDestructors};
    pThread->pDestructors = pDestructor;
    return 0;
}

// The NDK's prebuilt ARM libc++ uses legacy __sF offsets; new code imports the pointer globals.
unsigned char oy___sF[3 * 84] __attribute__((aligned(8)));
FILE *oy_stdin;
FILE *oy_stdout;
FILE *oy_stderr;
uintptr_t oy___stack_chk_guard = 0;

void oy_initialize_streams(void)
{
    oy_stdin = stdin;
    oy_stdout = stdout;
    oy_stderr = stderr;
}

FILE *oy_stream(FILE *pStream)
{
    if ((void *)pStream == oy___sF) return stdin;
    if ((void *)pStream == oy___sF + 84) return stdout;
    if ((void *)pStream == oy___sF + 168) return stderr;
    return pStream;
}

int oy_setvbuf(FILE *pStream, char *pBuffer, int mode, size_t size)
{
    // Android and VitaSDK both use modes 0/1/2; translate legacy standard stream handles.
    int result = setvbuf(oy_stream(pStream), pBuffer, mode, size);
    oy_sync_errno();
    return result;
}

int oy_fprintf(FILE *pStream, const char *pFormat, ...)
{
    va_list arguments;
    va_start(arguments, pFormat);
    int result;
    if (!oy_profile_capture_vprintf(oy_stream(pStream),pFormat,arguments,&result))
        result = vfprintf(oy_stream(pStream), pFormat, arguments);
    va_end(arguments);
    oy_sync_errno();
    return result;
}

void oy___stack_chk_fail(void)
{
    fprintf(stderr, "OpenYAMM: stack protector failure\n");
    fflush(stderr);
    abort();
}

static void checkBuffer(size_t requested, size_t available)
{
    if (requested > available)
    {
        fprintf(stderr, "OpenYAMM: fortified buffer check failed (%u > %u)\n", requested, available);
        abort();
    }
}

void *oy___memcpy_chk(void *pDestination, const void *pSource, size_t size, size_t capacity)
{
    checkBuffer(size, capacity);
    return memcpy(pDestination, pSource, size);
}

void *oy___memset_chk(void *pDestination, int value, size_t size, size_t capacity)
{
    checkBuffer(size, capacity);
    return memset(pDestination, value, size);
}

size_t oy___strlen_chk(const char *pString, size_t capacity)
{
    size_t size = strnlen(pString, capacity);
    checkBuffer(size + 1, capacity);
    return size;
}

char *oy___strcpy_chk(char *pDestination, const char *pSource, size_t capacity)
{
    checkBuffer(strlen(pSource) + 1, capacity);
    return strcpy(pDestination, pSource);
}

char *oy___strchr_chk(const char *pString, int character, size_t capacity)
{
    for (size_t index = 0; index < capacity; ++index)
    {
        if (pString[index] == (char)character) return (char *)pString + index;
        if (pString[index] == 0) return NULL;
    }
    abort();
}

char *oy___strncpy_chk2(char *pDestination, const char *pSource, size_t count,
    size_t destinationCapacity, size_t sourceCapacity)
{
    checkBuffer(count, destinationCapacity);
    if (count > sourceCapacity && strnlen(pSource, sourceCapacity) == sourceCapacity)
    {
        abort();
    }
    return strncpy(pDestination, pSource, count);
}

ssize_t oy_read(int descriptor, void *pBuffer, size_t size);

ssize_t oy___read_chk(int descriptor, void *pBuffer, size_t size, size_t capacity)
{
    checkBuffer(size, capacity);
    // Fortified and ordinary reads must dispatch the same virtual descriptors.
    return oy_read(descriptor, pBuffer, size);
}

int oy___vsnprintf_chk(char *pBuffer, size_t size, int flags, size_t capacity,
    const char *pFormat, va_list arguments)
{
    (void)flags;
    checkBuffer(size, capacity);
    return vsnprintf(pBuffer, size, pFormat, arguments);
}

int oy___android_log_write(int priority, const char *pTag, const char *pText)
{
    int result = fprintf(stderr, "Android[%d] %s: %s\n", priority, pTag, pText);
    fflush(stderr);
    return result < 0 ? -1 : 1;
}

void oy_android_set_abort_message(const char *pMessage)
{
    fprintf(stderr, "Android abort message: %s\n", pMessage);
    fflush(stderr);
}
