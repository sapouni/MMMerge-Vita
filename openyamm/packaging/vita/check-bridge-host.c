#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
static char *hostRealpath(const char *input, char *output)
{
    return _fullpath(output, input, 1024);
}
#define realpath hostRealpath
#endif
void oy_sync_errno(void) {}
unsigned char *oy_default_mbstate(void)
{
    static unsigned char state[4];
    return state;
}
#include "bridge-paths.c"
#include "bridge-multibyte.c"
int main(void)
{
    char path[1024];
    assert(strcmp(oy_native_path(oy_getenv("HOME"), path, sizeof(path)), "ux0:data/openyamm") == 0);
    assert(strcmp(oy_native_path(oy_getenv("TMPDIR"), path, sizeof(path)), "ux0:data/openyamm/tmp") == 0);
    assert(strcmp(oy_native_path("/app/assets/engine.zip", path, sizeof(path)),
        "app0:/assets/engine.zip") == 0);
    assert(strcmp(oy_native_path("/data/saves/a", path, sizeof(path)), "ux0:data/openyamm/saves/a") == 0);
    assert(strcmp(oy_native_path("/tmp/a", path, sizeof(path)), "ux0:data/openyamm/tmp/a") == 0);
    assert(strcmp(oy_native_path("saves/a", path, sizeof(path)), "saves/a") == 0);
    assert(strcmp(oy_native_path("/application", path, sizeof(path)), "/application") == 0);
    assert(oy_native_path("/app/a", path, 3) == NULL);
    assert(strcmp(virtualPath("ux0:data/openyamm/saves", path, sizeof(path)), "/data/saves") == 0);
    assert(setlocale(LC_ALL, "C.UTF-8") != NULL || setlocale(LC_ALL, ".UTF8") != NULL);
    unsigned char state[4] = {0};
    wchar_t character;
    assert(oy_mbrtowc(&character, "A", 1, state) == 1 && character == 'A');
    assert(oy_mbrtowc(&character, "\xe2", 1, state) == (size_t)-2);
    unsigned char copied[4];
    memcpy(copied, state, 4);
    assert(oy_mbrtowc(&character, "\x82\xac", 2, state) == 2 && character == 0x20ac);
    assert(oy_mbrtowc(&character, "\x82\xac", 2, copied) == 2 && character == 0x20ac);
    const char *input = "A\xe2\x82\xac";
    const char *original = input;
    assert(oy_mbsrtowcs(NULL, &input, 0, state) == 2 && input == original);
    wchar_t output[3];
    assert(oy_mbsrtowcs(output, &input, 3, state) == 2 && input == NULL);
    assert(output[0] == 'A' && output[1] == 0x20ac && output[2] == 0);
    puts("Path translation and ASCII/UTF-8 bridge checks passed.");
    return 0;
}
