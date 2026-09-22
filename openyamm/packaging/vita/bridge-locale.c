#define _GNU_SOURCE
#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

void oy_sync_errno(void);
int oy_translate_errno(int error);

locale_t oy_newlocale(int mask, const char *pName, locale_t base)
{
    int nativeMask = 0;
    if (mask & 1) nativeMask |= LC_CTYPE_MASK;
    if (mask & 2) nativeMask |= LC_NUMERIC_MASK;
    if (mask & 4) nativeMask |= LC_TIME_MASK;
    if (mask & 8) nativeMask |= LC_COLLATE_MASK;
    if (mask & 16) nativeMask |= LC_MONETARY_MASK;
    if (mask & 32) nativeMask |= LC_MESSAGES_MASK;
    if ((mask & ~63) != 0)
    {
        errno = EINVAL;
        oy_sync_errno();
        return NULL;
    }
    locale_t result = newlocale(nativeMask, pName, base);
    oy_sync_errno();
    return result;
}

typedef struct AndroidLocaleConv
{
    char *pStrings[10];
    char digits[14];
} AndroidLocaleConv;

AndroidLocaleConv *oy_localeconv(void)
{
    static AndroidLocaleConv result;
    const struct lconv *pNative = localeconv();
    memcpy(result.pStrings, pNative, sizeof(result.pStrings));
    memcpy(result.digits, &pNative->int_frac_digits, 8);
    result.digits[8] = pNative->int_p_cs_precedes;
    result.digits[9] = pNative->int_p_sep_by_space;
    result.digits[10] = pNative->int_n_cs_precedes;
    result.digits[11] = pNative->int_n_sep_by_space;
    result.digits[12] = pNative->int_p_sign_posn;
    result.digits[13] = pNative->int_n_sign_posn;
    return &result;
}

char *oy_strerror(int error)
{
    if (error >= 0 && error <= 34)
    {
        return strerror(error);
    }
    for (int native = 35; native <= 142; ++native)
    {
        if (oy_translate_errno(native) == error)
        {
            return strerror(native);
        }
    }
    return "Unknown error";
}

int oy_strerror_r(int error, char *pBuffer, size_t capacity)
{
    const char *pText = oy_strerror(error);
    size_t size = strlen(pText);
    if (capacity == 0)
    {
        return 34;
    }
    size_t copied = size < capacity - 1 ? size : capacity - 1;
    memcpy(pBuffer, pText, copied);
    pBuffer[copied] = 0;
    return copied == size ? 0 : 34;
}
