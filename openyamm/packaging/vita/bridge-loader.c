#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include "so_util.h"

void oy_sync_errno(void);
static const so_module *pLoadedGame;
extern const uint32_t __exidx_start[];
extern const uint32_t __exidx_end[];

void oy_set_loaded_game(const so_module *pModule)
{
    pLoadedGame = pModule;
}

const void *oy_dl_unwind_find_exidx(uintptr_t address, int *pCount)
{
    if (pLoadedGame != NULL && address >= pLoadedGame->text_base &&
        address < pLoadedGame->text_base + pLoadedGame->text_size)
    {
        *pCount = pLoadedGame->exidx_size / 8;
        return (const void *)pLoadedGame->exidx_base;
    }
    *pCount = (__exidx_end - __exidx_start) / 2;
    return __exidx_start;
}

int oy_dlclose(void *pHandle)
{
    (void)pHandle;
    errno = EINVAL;
    oy_sync_errno();
    return -1;
}
