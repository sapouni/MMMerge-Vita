#pragma once
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>
extern int oy_game_thread_priority;
extern int oy_render_thread_priority;
void oy_stall_register_thread(const char *role);
static void oy_pin_thread(const char *role, int mask)
{
    SceUID thread = sceKernelGetThreadId();
    oy_stall_register_thread(role);
    if (strcmp(role, "game") == 0)
    {
        SceKernelThreadInfo info = {0}; info.size = sizeof(info);
        if (sceKernelGetThreadInfo(thread, &info) == 0)
        { __atomic_store_n(&oy_game_thread_priority, info.currentPriority, __ATOMIC_RELEASE); }
    }
    if (strcmp(role, "render") == 0)
    {
        SceKernelThreadInfo info = {0}; info.size = sizeof(info);
        if (sceKernelGetThreadInfo(thread, &info) == 0)
        { __atomic_store_n(&oy_render_thread_priority, info.currentPriority, __ATOMIC_RELEASE); }
    }
    int result = sceKernelChangeThreadCpuAffinityMask(thread, mask);
    fprintf(stderr, "VITA AFFINITY role=%s thread=%d requested=0x%x actual=0x%x result=0x%x\n",
        role, thread, mask, sceKernelGetThreadCpuAffinityMask(thread), result);
}
static void oy_background_thread(const char *role)
{
    const SceUID thread = sceKernelGetThreadId();
    SceKernelThreadInfo info = {0}; info.size = sizeof(info);
    const int query = sceKernelGetThreadInfo(thread, &info);
    // Vita priorities are inverse: larger numbers run after the foreground threads.
    const int game = __atomic_load_n(&oy_game_thread_priority, __ATOMIC_ACQUIRE);
    const int render = __atomic_load_n(&oy_render_thread_priority, __ATOMIC_ACQUIRE);
    const int priority = (game > render ? game : render) + 16;
    const int changed = sceKernelChangeThreadPriority(thread, priority);
    oy_pin_thread(role, SCE_KERNEL_CPU_MASK_USER_2);
    fprintf(stderr, "VITA BACKGROUND role=%s old_priority=%d requested=%d result=%d\n",
        role, query == 0 ? info.currentPriority : query, priority, changed);
}
