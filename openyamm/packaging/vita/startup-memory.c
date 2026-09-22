#include <stdlib.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

extern unsigned int _newlib_heap_size_user;

// Newlib initializes its allocator before this runs; C++ global constructors run afterwards.
static void inspectStartupMemory(void)
{
    SceKernelFreeMemorySizeInfo available = {sizeof(available), 0, 0, 0};
    int queryResult = sceKernelGetFreeMemorySize(&available);
    void *probe = malloc(64);
    char text[256];
    int length = sceClibSnprintf(text, sizeof(text),
        "GAME PREINIT 00.70: heap=%u probe=%s free_query=%08x user=%d cdram=%d phycont=%d\n",
        _newlib_heap_size_user, probe != NULL ? "OK" : "FAILED", queryResult,
        available.size_user, available.size_cdram, available.size_phycont);
    SceUID file = sceIoOpen("ux0:data/openyamm-boot.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (file >= 0)
    {
        if (length > 0 && length < (int)sizeof(text)) sceIoWrite(file, text, length);
        sceIoClose(file);
    }
    if (probe == NULL)
    {
        // Avoid an opaque C++ constructor abort after recording the allocator failure.
        sceKernelExitProcess(1);
        return;
    }
    free(probe);
}

__attribute__((used, section(".preinit_array")))
static void (*const memoryPreinit)(void) = inspectStartupMemory;
















