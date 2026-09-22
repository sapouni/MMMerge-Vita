#define _GNU_SOURCE
#include "android-abi.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

void oy_sync_errno(void);

typedef struct AndroidTm
{
    int seconds, minutes, hours, day, month, year, weekday, yearday, daylight;
    int32_t offset;
    const char *pZone;
} AndroidTm;

static void fromNativeTm(const struct tm *pNative, AndroidTm *pAndroid)
{
    memcpy(pAndroid, pNative, 9 * sizeof(int));
    pAndroid->offset = 0;
    pAndroid->pZone = "UTC";
}

int oy_clock_gettime(int clockId, AndroidTimespec *pResult)
{
    if (clockId == 1 || clockId == 4 || clockId == 6 || clockId == 7)
    {
        uint64_t microseconds = sceKernelGetProcessTimeWide();
        pResult->seconds = microseconds / 1000000;
        pResult->nanoseconds = (microseconds % 1000000) * 1000;
        return 0;
    }
    if (clockId == 0 || clockId == 5)
    {
        struct timeval current;
        int result = gettimeofday(&current, NULL);
        if (result == 0)
        {
            pResult->seconds = current.tv_sec;
            pResult->nanoseconds = current.tv_usec * 1000;
        }
        oy_sync_errno();
        return result;
    }
    errno = EINVAL;
    oy_sync_errno();
    return -1;
}

int oy_gettimeofday(AndroidTimespec *pResult, void *pTimezone)
{
    (void)pTimezone;
    struct timeval current;
    int result = gettimeofday(&current, NULL);
    if (result == 0 && pResult != NULL)
    {
        pResult->seconds = current.tv_sec;
        // timeval's second member is microseconds, despite reusing the layout type here.
        pResult->nanoseconds = current.tv_usec;
    }
    oy_sync_errno();
    return result;
}

int oy_nanosleep(const AndroidTimespec *pDuration, AndroidTimespec *pRemaining)
{
    struct timespec duration = {pDuration->seconds, pDuration->nanoseconds};
    struct timespec remaining = {0};
    int result = nanosleep(&duration, &remaining);
    if (result != 0 && pRemaining != NULL)
    {
        pRemaining->seconds = remaining.tv_sec;
        pRemaining->nanoseconds = remaining.tv_nsec;
    }
    oy_sync_errno();
    return result;
}

int32_t oy_time(int32_t *pResult)
{
    time_t current = time(NULL);
    if (current > INT32_MAX)
    {
        errno = EOVERFLOW;
        oy_sync_errno();
        return -1;
    }
    if (pResult != NULL)
    {
        *pResult = current;
    }
    return current;
}

AndroidTm *oy_gmtime_r(const int32_t *pTime, AndroidTm *pResult)
{
    time_t input = *pTime;
    struct tm result;
    if (gmtime_r(&input, &result) == NULL)
    {
        oy_sync_errno();
        return NULL;
    }
    fromNativeTm(&result, pResult);
    return pResult;
}

AndroidTm *oy_localtime_r(const int32_t *pTime, AndroidTm *pResult)
{
    time_t input = *pTime;
    struct tm result;
    if (localtime_r(&input, &result) == NULL)
    {
        oy_sync_errno();
        return NULL;
    }
    fromNativeTm(&result, pResult);
    // VitaSDK's tm has no UTC-offset/zone members. Derive the offset using its timezone rules.
    struct tm utc;
    gmtime_r(&input, &utc);
    utc.tm_isdst = result.tm_isdst;
    pResult->offset = input - mktime(&utc);
    pResult->pZone = tzname[result.tm_isdst > 0];
    return pResult;
}

int32_t oy_mktime(AndroidTm *pTime)
{
    struct tm input = {0};
    memcpy(&input, pTime, 9 * sizeof(int));
    time_t result = mktime(&input);
    if (result > INT32_MAX || result < INT32_MIN)
    {
        errno = EOVERFLOW;
        oy_sync_errno();
        return -1;
    }
    fromNativeTm(&input, pTime);
    oy_sync_errno();
    return result;
}

size_t oy_strftime(char *pBuffer, size_t capacity, const char *pFormat, const AndroidTm *pTime)
{
    struct tm input = {0};
    memcpy(&input, pTime, 9 * sizeof(int));
    return strftime(pBuffer, capacity, pFormat, &input);
}

size_t oy_strftime_l(char *pBuffer, size_t capacity, const char *pFormat,
    const AndroidTm *pTime, locale_t locale)
{
    struct tm input = {0};
    memcpy(&input, pTime, 9 * sizeof(int));
    return strftime_l(pBuffer, capacity, pFormat, &input, locale);
}
