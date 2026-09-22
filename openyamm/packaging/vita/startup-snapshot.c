// Temporary diagnostic: preserve all thread contexts with an intentional core dump.
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

static SceUID startupThread;

static int captureStartup(SceSize argumentSize, void *pArguments)
{
    (void)argumentSize;
    (void)pArguments;
    for (int sample = 1; sample <= 3; ++sample)
    {
        sceKernelDelayThread(15000000);
        SceKernelThreadInfo info = {0};
        info.size = sizeof(info);
        SceKernelFreeMemorySizeInfo memory = {sizeof(memory), 0, 0, 0};
        const int threadResult = sceKernelGetThreadInfo(startupThread, &info);
        const int memoryResult = sceKernelGetFreeMemorySize(&memory);
        char text[512];
        const int length = sceClibSnprintf(text, sizeof(text),
            "SNAPSHOT seconds=%d thread=%08x query=%08x status=%08x wait_type=%08x wait_id=%08x "
            "cpu_time=%llu stack=%p size=%d memory_query=%08x user=%d cdram=%d phycont=%d\n",
            sample * 15, startupThread, threadResult, info.status, info.waitType, info.waitId,
            (unsigned long long)info.runClocks, info.stack, info.stackSize, memoryResult,
            memory.size_user, memory.size_cdram, memory.size_phycont);
        // Use kernel I/O and a stack buffer: do not depend on guest/native stdio or allocator locks.
        const SceUID file = sceIoOpen("ux0:data/openyamm/snapshot.log",
            SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
        if (file >= 0)
        {
            if (length > 0 && length < (int)sizeof(text)) sceIoWrite(file, text, length);
            if (sample == 3)
            {
                const char note[] = "Intentional diagnostic trap in OYAMSnapshot; inspect OTHER threads in core.\n";
                sceIoWrite(file, note, sizeof(note) - 1);
            }
            sceIoClose(file);
        }
    }
    __builtin_trap();
    return 0;
}

int oy_start_startup_snapshot(void)
{
    startupThread = sceKernelGetThreadId();
    const SceUID file = sceIoOpen("ux0:data/openyamm/snapshot.log",
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (file >= 0) sceIoClose(file);
    const SceUID thread = sceKernelCreateThread("OYAMSnapshot", captureStartup, 0x40, 16384, 0, 0, NULL);
    if (thread < 0) return thread;
    const int result = sceKernelStartThread(thread, 0, NULL);
    if (result < 0) sceKernelDeleteThread(thread);
    return result;
}
