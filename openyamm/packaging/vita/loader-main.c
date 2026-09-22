#include "vita-affinity.h"
int oy_game_thread_priority = 256;
int oy_render_thread_priority = 0;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <SDL3/SDL.h>
#include <taihen.h>
#include <psp2/vshbridge.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/appmgr.h>
#include <vitaGL.h>
#include "so_util.h"

// Leave room for the loaded ARM image, Vita graphics allocations and system modules.
// Existing pool split: reduced bgfx transient storage transfers 8 MiB of graphics RAM to newlib.
// The combined reservation, CDRAM, PHYCONT and kernel/thread reserve are unchanged.
unsigned int _newlib_heap_size_user = 140 * 1024 * 1024;
static so_module gameModule;

const so_default_dynlib *oy_get_symbols(size_t *pCount);
void (*oy_eglGetProcAddress(const char *pName))(void);
void oy_initialize_streams(void);
void oy_set_loaded_game(const so_module *pModule);
extern uintptr_t oy___stack_chk_guard;

static int reportFailure(const char *pMessage)
{
    fprintf(stderr, "%s\n", pMessage);
    fflush(stderr);
    // SDL's GXM dialog renderer does not own the vitaGL context. Keep failure handling graphics-free.
    sceKernelExitProcess(1);
    return 1;
}

int oy_allocator_start(void);
void oy_allocator_queue_state(unsigned *, unsigned *, unsigned *, int *);
void oy_allocator_snapshot(char *, unsigned);
int main(void)
{
    const int allocatorStart = oy_allocator_start();
    SceUID earlyLog = sceIoOpen("ux0:data/openyamm-boot.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (earlyLog >= 0)
    {
        const char entered[] = "GAME LOADER MAIN ENTERED\n";
        sceIoWrite(earlyLog, entered, sizeof(entered) - 1);
        sceIoClose(earlyLog);
    }
    mkdir("ux0:data/openyamm", 0777);
    mkdir("ux0:data/openyamm/tmp", 0777);
    // Preserve the previous launch even when a later launch exits before gameplay capture.
    const char *previousLogs[] = {"loader", "game"};
    for (unsigned i = 0; i < 2; ++i)
    {
        char previous[128], archived[192]; struct stat previousStatus;
        snprintf(previous, sizeof(previous), "ux0:data/openyamm/%s.log", previousLogs[i]);
        if (stat(previous, &previousStatus) == 0 && previousStatus.st_size > 0)
        {
            snprintf(archived, sizeof(archived), "ux0:data/openyamm/%s-before-%llu-%llu.log",
                previousLogs[i], (unsigned long long)time(NULL), (unsigned long long)sceKernelGetProcessTimeWide());
            rename(previous, archived);
        }
    }
    FILE *pLog = freopen("ux0:data/openyamm/loader.log", "w", stderr);
    if (pLog == NULL)
    {
        return 1;
    }
    static char loaderBuffer[8192], gameBuffer[8192];
    setvbuf(stderr, loaderBuffer, _IOLBF, sizeof(loaderBuffer));
    freopen("ux0:data/openyamm/game.log", "w", stdout);
    setvbuf(stdout, gameBuffer, _IOLBF, sizeof(gameBuffer));
    unsigned heapSeen, heapRequested, heapActual; int heapError;
    oy_allocator_queue_state(&heapSeen, &heapRequested, &heapActual, &heapError);
    fprintf(stderr,"HEAP QUEUE original-lwmutex seen=%u requested-priority=%u actual-priority=%u fallback-error=0x%x\n",
        heapSeen,heapRequested,heapActual,heapError);
    fprintf(stderr, "OpenYAMM Vita loader 01.06; projectile broad phase and lazy area facts; cached terrain mips; refitted collider index; vitaGL SAFE_DRAW=1\n");
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    fprintf(stderr, "VITA CLOCKS: cpu=%d bus=%d gpu=%d xbar=%d MHz\n",
        scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
        scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    extern int oy_render_width, oy_render_height;
    FILE *resolutionFile = fopen("ux0:data/openyamm/vita-performance.ini", "r");
    if (resolutionFile != NULL)
    {
        char line[256]; unsigned value;
        while (fgets(line, sizeof(line), resolutionFile))
        {
            if (sscanf(line, " render_resolution = %u", &value) == 1 && (value == 0 || value == 1))
            { oy_render_width = value ? 720 : 960; oy_render_height = value ? 408 : 544; }
        }
        fclose(resolutionFile);
    }
    fprintf(stderr, "VITA framebuffer=%dx%d logical=960x544 (render_resolution option)\n",
        oy_render_width, oy_render_height);
    char allocatorInfo[512]; oy_allocator_snapshot(allocatorInfo, sizeof(allocatorInfo));
    fprintf(stderr, "VITA ALLOCATOR_START result=0x%x %s", allocatorStart, allocatorInfo);
    oy_initialize_streams();
    SceAppMgrBudgetInfo budget = {0}; budget.size = sizeof(budget);
    int budgetResult = sceAppMgrGetBudgetInfo(&budget);
    if (budgetResult == 0)
        fprintf(stderr, "VITA APP_BUDGET user=%u extra=%u cdram=%u phycont=%u\n",
            budget.total_user_rw_mem, budget.total_extra_mem, budget.total_cdram_mem, budget.total_phycont_mem);
    else fprintf(stderr, "VITA APP_BUDGET unavailable result=0x%x; use kernel free and block accounting\n", budgetResult);
    fprintf(stderr, "STAGE: streams initialized; starting SDL\n");
    oy___stack_chk_guard = (uintptr_t)&gameModule ^ (uintptr_t)time(NULL) ^ 0x6a4d8319u;
    SDL_SetHint(SDL_HINT_VITA_ENABLE_BACK_TOUCH, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    fprintf(stderr, "STAGE: SDL initialized; checking prerequisites\n");
    int searchBuffer[2] = {0};
    if (_vshKernelSearchModuleByName("kubridge", searchBuffer) < 0)
    {
        return reportFailure("kubridge is required. Details: ux0:data/openyamm/loader.log");
    }
    struct stat compilerStatus;
    if (stat("ur0:data/libshacccg.suprx", &compilerStatus) != 0 &&
        stat("ur0:data/external/libshacccg.suprx", &compilerStatus) != 0)
    {
        return reportFailure("libshacccg.suprx is required in ur0:data or ur0:data/external.");
    }
    fprintf(stderr, "STAGE: loading Android library\n");
    if (so_file_load(&gameModule, "app0:libmain.so", 0x98000000) < 0)
    {
        return reportFailure("The OpenYAMM ARM library could not be loaded. See loader.log.");
    }
    size_t baseCount;
    fprintf(stderr, "STAGE: Android library loaded; resolving imports\n");
    const so_default_dynlib *pBase = oy_get_symbols(&baseCount);
    so_default_dynlib *pSymbols = calloc(baseCount + gameModule.num_dynsym, sizeof(*pSymbols));
    if (pSymbols == NULL)
    {
        return reportFailure("Not enough memory for the import table.");
    }
    memcpy(pSymbols, pBase, baseCount * sizeof(*pSymbols));
    size_t symbolCount = baseCount;
    size_t missingCount = 0;
    for (uint32_t index = 0; index < gameModule.num_dynsym; ++index)
    {
        const Elf32_Sym *pSymbol = &gameModule.dynsym[index];
        if (pSymbol->st_shndx != SHN_UNDEF || pSymbol->st_name == 0)
        {
            continue;
        }
        const char *pName = gameModule.dynstr + pSymbol->st_name;
        int found = 0;
        for (size_t entry = 0; entry < symbolCount; ++entry)
        {
            if (strcmp(pName, pSymbols[entry].symbol) == 0)
            {
                found = 1;
                break;
            }
        }
        if (!found && (strncmp(pName, "gl", 2) == 0 || strncmp(pName, "egl", 3) == 0))
        {
            void *pFunction = (void *)oy_eglGetProcAddress(pName);
            if (pFunction != NULL)
            {
                pSymbols[symbolCount++] = (so_default_dynlib){pName, (uintptr_t)pFunction};
                found = 1;
            }
        }
        if (!found)
        {
            fprintf(stderr, "Unresolved import: %s\n", pName);
            ++missingCount;
        }
    }
    if (missingCount != 0)
    {
        free(pSymbols);
        char message[256];
        snprintf(message, sizeof(message),
            "The port still lacks %u imported functions. The game has not been started. See loader.log.",
            (unsigned int)missingCount);
        return reportFailure(message);
    }
#if OY_PROFILE_ENABLED
    extern void *oy_profile_wrap(const char *, void *);
    for (size_t i = 0; i < symbolCount; ++i)
    {
        pSymbols[i].func = (uintptr_t)oy_profile_wrap(pSymbols[i].symbol, (void *)pSymbols[i].func);
    }
#endif
    so_relocate(&gameModule);
    fprintf(stderr, "STAGE: relocations applied\n");
    so_resolve(&gameModule, pSymbols, symbolCount * sizeof(*pSymbols), 1);
    free(pSymbols);
    so_flush_caches(&gameModule);
    oy_set_loaded_game(&gameModule);
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
    vglSetShaderCachePath("ux0:data/openyamm/shader-cache-v1");
    fprintf(stderr, "STAGE: starting vitaGL\n");
    // vitaGL returns whether it reduced the requested resolution, not a success boolean.
    GLboolean resolutionChanged = vglInitWithCustomSizes(0, oy_render_width, oy_render_height, 60 * 1024 * 1024, 96 * 1024 * 1024,
        26 * 1024 * 1024, 0, SCE_GXM_MULTISAMPLE_NONE);
    if (resolutionChanged)
    {
        fprintf(stderr, "vitaGL reduced the requested display resolution.\n");
    }
    // The first actual frame makes vitaGL join its splash worker through scene_reset.
    // Complete that handover before the game can fail or exit during asset initialization.
    fprintf(stderr, "STAGE: vitaGL initialized; completing splash handover\n");
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    vglSwapBuffers(GL_FALSE);
#if OY_PROFILE_ENABLED
    extern void oy_profile_memory_init(void);
    oy_profile_memory_init();
#endif
    fprintf(stderr, "STAGE: splash handover completed; running Android constructors\n");
    so_initialize(&gameModule);
    fprintf(stderr, "STAGE: Android constructors completed\n");
    extern void oy_log_memory_snapshot(const char *stage);
    int (*pEntry)(const char *, void (*)(const char *), uint32_t, void (*)(uint32_t, uint64_t)) = (void *)so_symbol(&gameModule, "openyamm_vita_main");
    if (pEntry == NULL)
    {
        return reportFailure("The ARM library has no Vita entry point.");
    }
    size_t guestDataBytes = 0;
    for (unsigned i = 0; i < MAX_DATA_SEG; ++i) guestDataBytes += gameModule.data_size[i];
#if OY_PROFILE_ENABLED
    oy_log_memory_snapshot("native pools and guest ready");
#endif
    fprintf(stderr, "VITA MEMORY_LAYOUT newlib_MiB=140 vitaGL_RAM_MiB=60 CDRAM_MiB=96 PHYCONT_MiB=26 "
        "guest_text=%u guest_data=%u guest_patch=%u\n", gameModule.text_size, guestDataBytes, gameModule.patch_size);
    fprintf(stderr, "STAGE: entering game\n");
#if OY_PROFILE_ENABLED
    extern void oy_profile_event(uint32_t, uint64_t);
    const uint32_t mode = OY_PROFILE_NO_AUDIO | (OY_PROFILE_UI_ONLY << 1);
    fprintf(stderr, "PROFILE MODE no_audio=%u ui_only=%u baseline=00.29 variant=01.06\n",
        OY_PROFILE_NO_AUDIO, OY_PROFILE_UI_ONLY);
    oy_pin_thread("game", SCE_KERNEL_CPU_MASK_USER_0);
    int result = pEntry("/data", oy_log_memory_snapshot, mode, oy_profile_event);
#else
    fprintf(stderr, "BUILD 01.06-clean: general frame profiler off; binary incident diagnostics opt-in\n");
#if defined(OY_PC_SAMPLER)
    extern void oy_sampler_start(uint32_t);
    oy_sampler_start(0x98000000);
#endif
    oy_pin_thread("game", SCE_KERNEL_CPU_MASK_USER_0);
    int result = pEntry("/data", NULL, 0, NULL);
#endif
    fprintf(stderr, "Game returned %d\n", result);
    // The loaded module and vitaGL workers have process lifetime. Do not run native
    // static destructors while the graphics workers can still access their state.
    fflush(NULL);
    sceKernelExitProcess(result);
    return result;
}
