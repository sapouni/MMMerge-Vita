#pragma once
#include <stddef.h>
#include <stdint.h>

/* IME needs UTF-16; Vita newlib's iconv tables are not populated in this toolchain.
 * Bounded conversion uses caller storage, including room for the terminator. */
static size_t oy_ime_to_utf16(uint16_t *dest, size_t capacity, const char *text)
{
    size_t written = 0;
    const unsigned char *p = (const unsigned char *)text;
    if (!capacity) return 0;
    while (*p)
    {
        uint32_t value = *p++;
        unsigned extra = 0;
        uint32_t minimum = 0;
        if (value >= 0xc2 && value <= 0xdf) { value &= 0x1f; extra = 1; minimum = 0x80; }
        else if (value >= 0xe0 && value <= 0xef) { value &= 0x0f; extra = 2; minimum = 0x800; }
        else if (value >= 0xf0 && value <= 0xf4) { value &= 7; extra = 3; minimum = 0x10000; }
        else if (value >= 0x80) value = 0xfffd;
        unsigned i = 0;
        for (; i < extra && (*p & 0xc0) == 0x80; ++i) value = (value << 6) | (*p++ & 0x3f);
        if (i != extra || value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            value = 0xfffd;
        const size_t units = value >= 0x10000 ? 2 : 1;
        if (written + units >= capacity) break;
        if (units == 2)
        {
            value -= 0x10000;
            dest[written++] = (uint16_t)(0xd800 | (value >> 10));
            dest[written++] = (uint16_t)(0xdc00 | (value & 0x3ff));
        }
        else dest[written++] = (uint16_t)value;
    }
    dest[written] = 0;
    return written;
}

static size_t oy_ime_to_utf8(char *dest, size_t capacity, const uint16_t *text, size_t maxUnits)
{
    size_t written = 0;
    if (!capacity) return 0;
    for (size_t i = 0; i < maxUnits && text[i]; ++i)
    {
        uint32_t value = text[i];
        if (value >= 0xd800 && value <= 0xdbff)
        {
            if (i + 1 < maxUnits && text[i+1] >= 0xdc00 && text[i+1] <= 0xdfff)
                value = 0x10000 + ((value - 0xd800) << 10) + (text[++i] - 0xdc00);
            else value = 0xfffd;
        }
        else if (value >= 0xdc00 && value <= 0xdfff) value = 0xfffd;
        const unsigned bytes = value < 0x80 ? 1 : value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
        if (written + bytes >= capacity) break;
        if (bytes == 1) dest[written++] = (char)value;
        else
        {
            dest[written++] = (char)((bytes == 2 ? 0xc0 : bytes == 3 ? 0xe0 : 0xf0)
                | (value >> (6 * (bytes - 1))));
            for (unsigned j = bytes - 1; j > 0; --j)
                dest[written++] = (char)(0x80 | ((value >> (6 * (j - 1))) & 0x3f));
        }
    }
    dest[written] = 0;
    return written;
}
