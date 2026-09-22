#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <psp2/kernel/rng.h>

// Guest-only descriptors, outside the native newlib descriptor range. Each open
// owns a slot; closing one random_device must not invalidate another instance.
enum { RandomDescriptorBase = 0x40000000, RandomDescriptorCount = 32 };
static unsigned char randomDescriptors[RandomDescriptorCount];
static pthread_mutex_t randomDescriptorMutex = PTHREAD_MUTEX_INITIALIZER;

static int randomDescriptorIndex(int descriptor)
{
    return descriptor >= RandomDescriptorBase && descriptor < RandomDescriptorBase + RandomDescriptorCount
        ? descriptor - RandomDescriptorBase : -1;
}

static int randomDescriptorIsOpen(int descriptor)
{
    int index = randomDescriptorIndex(descriptor);
    if (index < 0) return 0;
    pthread_mutex_lock(&randomDescriptorMutex);
    int active = randomDescriptors[index];
    pthread_mutex_unlock(&randomDescriptorMutex);
    return active;
}

static int openRandomDescriptor(int flags)
{
    // Android O_RDONLY, O_NONBLOCK, O_LARGEFILE and O_CLOEXEC. The guest does not exec.
    if (flags & ~(0x800 | 0x8000 | 0x80000))
    {
        errno = EACCES;
        return -1;
    }
    pthread_mutex_lock(&randomDescriptorMutex);
    for (int index = 0; index < RandomDescriptorCount; ++index)
    {
        if (!randomDescriptors[index])
        {
            randomDescriptors[index] = 1;
            pthread_mutex_unlock(&randomDescriptorMutex);
            return RandomDescriptorBase + index;
        }
    }
    pthread_mutex_unlock(&randomDescriptorMutex);
    errno = EMFILE;
    return -1;
}

static int closeRandomDescriptor(int descriptor)
{
    int index = randomDescriptorIndex(descriptor);
    pthread_mutex_lock(&randomDescriptorMutex);
    int active = index >= 0 && randomDescriptors[index];
    if (active) randomDescriptors[index] = 0;
    pthread_mutex_unlock(&randomDescriptorMutex);
    if (!active) errno = EBADF;
    return active ? 0 : -1;
}

static ssize_t readRandomDescriptor(int descriptor, void *buffer, size_t size)
{
    if (!randomDescriptorIsOpen(descriptor))
    {
        errno = EBADF;
        return -1;
    }
    const size_t maxReadSize = SIZE_MAX >> 1;
    if (size > maxReadSize || (!buffer && size))
    {
        errno = size > maxReadSize ? EINVAL : EFAULT;
        return -1;
    }
    size_t written = 0;
    while (written < size)
    {
        size_t chunk = size - written > 64 ? 64 : size - written;
        if (sceKernelGetRandomNumber((unsigned char *)buffer + written, chunk) < 0)
        {
            errno = EIO;
            return written ? (ssize_t)written : -1;
        }
        written += chunk;
    }
    return (ssize_t)written;
}
