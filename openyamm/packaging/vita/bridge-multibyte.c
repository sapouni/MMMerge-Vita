#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

void oy_sync_errno(void);
unsigned char *oy_default_mbstate(void);

size_t oy_mbrtowc(wchar_t *pOutput, const char *pInput, size_t count, void *pState)
{
    unsigned char *pSaved = pState != NULL ? pState : oy_default_mbstate();
    unsigned char bytes[4];
    unsigned int previous = pSaved[3];
    if (previous > 3)
    {
        errno = EILSEQ;
        oy_sync_errno();
        return (size_t)-1;
    }
    memcpy(bytes, pSaved, previous);
    if (pInput == NULL)
    {
        pInput = "";
        count = 1;
        pOutput = NULL;
    }
    size_t consumed = 0;
    while (consumed < count && previous + consumed < sizeof(bytes))
    {
        bytes[previous + consumed] = pInput[consumed];
        ++consumed;
        mbstate_t nativeState = {0};
        wchar_t character;
        size_t result = mbrtowc(&character, (const char *)bytes, previous + consumed, &nativeState);
        if (result == (size_t)-1)
        {
            memset(pSaved, 0, 4);
            oy_sync_errno();
            return result;
        }
        if (result != (size_t)-2)
        {
            memset(pSaved, 0, 4);
            if (pOutput != NULL)
            {
                *pOutput = character;
            }
            return result == 0 ? 0 : consumed;
        }
    }
    if (previous + consumed == 4)
    {
        errno = EILSEQ;
        oy_sync_errno();
        return (size_t)-1;
    }
    memcpy(pSaved, bytes, previous + consumed);
    pSaved[3] = previous + consumed;
    return (size_t)-2;
}

size_t oy_mbrlen(const char *pInput, size_t count, void *pState)
{
    return oy_mbrtowc(NULL, pInput, count, pState);
}

int oy_mbtowc(wchar_t *pOutput, const char *pInput, size_t count)
{
    unsigned char state[4] = {0};
    if (pInput == NULL)
    {
        return 0;
    }
    size_t result = oy_mbrtowc(pOutput, pInput, count, state);
    return result == (size_t)-2 ? -1 : (int)result;
}

size_t oy_wcrtomb(char *pOutput, wchar_t character, void *pState)
{
    unsigned char *pSaved = pState != NULL ? pState : oy_default_mbstate();
    mbstate_t nativeState = {0};
    char reset[MB_LEN_MAX];
    if (pOutput == NULL)
    {
        pOutput = reset;
        character = 0;
    }
    size_t result = wcrtomb(pOutput, character, &nativeState);
    memset(pSaved, 0, 4);
    oy_sync_errno();
    return result;
}

size_t oy_mbsnrtowcs(wchar_t *pOutput, const char **ppInput, size_t inputBytes,
    size_t capacity, void *pState)
{
    unsigned char localState[4];
    void *pCurrentState = pState != NULL ? pState : oy_default_mbstate();
    if (pOutput == NULL)
    {
        memcpy(localState, pCurrentState, 4);
        pCurrentState = localState;
        capacity = SIZE_MAX;
    }
    const char *pCurrent = *ppInput;
    size_t converted = 0;
    while (inputBytes != 0 && converted < capacity)
    {
        wchar_t character;
        size_t consumed = oy_mbrtowc(&character, pCurrent, inputBytes, pCurrentState);
        if (consumed == (size_t)-1)
        {
            if (pOutput != NULL) *ppInput = pCurrent;
            return (size_t)-1;
        }
        if (consumed == (size_t)-2)
        {
            pCurrent += inputBytes;
            break;
        }
        if (pOutput != NULL) pOutput[converted] = character;
        if (consumed == 0)
        {
            if (pOutput != NULL) *ppInput = NULL;
            return converted;
        }
        pCurrent += consumed;
        inputBytes -= consumed;
        ++converted;
    }
    if (pOutput != NULL) *ppInput = pCurrent;
    return converted;
}

size_t oy_mbsrtowcs(wchar_t *pOutput, const char **ppInput, size_t capacity, void *pState)
{
    return oy_mbsnrtowcs(pOutput, ppInput, SIZE_MAX, capacity, pState);
}

size_t oy_wcsnrtombs(char *pOutput, const wchar_t **ppInput, size_t inputCharacters,
    size_t capacity, void *pState)
{
    unsigned char localState[4];
    void *pCurrentState = pState != NULL ? pState : oy_default_mbstate();
    if (pOutput == NULL)
    {
        memcpy(localState, pCurrentState, 4);
        pCurrentState = localState;
        capacity = SIZE_MAX;
    }
    const wchar_t *pCurrent = *ppInput;
    size_t converted = 0;
    while (inputCharacters != 0)
    {
        char bytes[MB_LEN_MAX];
        size_t length = oy_wcrtomb(bytes, *pCurrent, pCurrentState);
        if (length == (size_t)-1)
        {
            if (pOutput != NULL) *ppInput = pCurrent;
            return length;
        }
        if (length > capacity - converted) break;
        if (pOutput != NULL) memcpy(pOutput + converted, bytes, length);
        if (*pCurrent == 0)
        {
            if (pOutput != NULL) *ppInput = NULL;
            return converted + length - 1;
        }
        converted += length;
        ++pCurrent;
        --inputCharacters;
    }
    if (pOutput != NULL) *ppInput = pCurrent;
    return converted;
}

size_t oy___ctype_get_mb_cur_max(void)
{
    return MB_CUR_MAX;
}
