#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/vshbridge.h>
#include "first-run-files.h"
#include "asset-location.h"

// Keep the diagnostic process independent of the game's heap, SDL and graphics libraries.
unsigned int _newlib_heap_size_user = 4 * 1024 * 1024;
static uint32_t *pixels;
static int lineY = 24;
static const unsigned char glyphs[][7] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,25,21,19,19,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14},{0,4,4,0,4,4,0},{0,0,0,0,0,4,4},
    {1,2,2,4,8,8,16},{0,0,0,31,0,0,0}
};
static const char glyphNames[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:./-";

static void logLine(const char *text)
{
    SceUID file = sceIoOpen("ux0:data/openyamm-boot.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (file >= 0)
    {
        sceIoWrite(file, text, strlen(text));
        sceIoWrite(file, "\n", 1);
        sceIoClose(file);
    }
}

static void showLine(const char *text)
{
    logLine(text);
    if (pixels == NULL) return;
    int x = 24;
    for (const char *p = text; *p && x + 15 < 960; ++p, x += 18)
    {
        char c = *p >= 'a' && *p <= 'z' ? *p - 'a' + 'A' : *p;
        const char *position = strchr(glyphNames, c);
        if (position == NULL) continue;
        const unsigned char *glyph = glyphs[position - glyphNames];
        for (int y = 0; y < 7; ++y)
        {
            for (int col = 0; col < 5; ++col)
            {
                if (!(glyph[y] & (1 << (4 - col)))) continue;
                for (int dy = 0; dy < 3; ++dy)
                    for (int dx = 0; dx < 3; ++dx)
                        if (lineY + y * 3 + dy < 544)
                            pixels[(lineY + y * 3 + dy) * 960 + x + col * 3 + dx] = 0xffffffff;
            }
        }
    }
    lineY += 30;
}

static int filePresent(const char *path)
{
    SceIoStat status;
    return sceIoGetstat(path, &status) >= 0 && status.st_size > 0;
}

int main(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/openyamm", 0777);
    SceUID log = sceIoOpen("ux0:data/openyamm-boot.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (log >= 0) sceIoClose(log);
    logLine("BOOTSTRAP MAIN ENTERED - BUILD 01.06 INSTALL R1; GAME 01.06 CLEAN");
    const char *defaultFiles[] = {"settings.ini", "vita-performance.ini", "vita-guide.txt"};
    int defaultsReady = 1;
    for (unsigned i = 0; i < sizeof(defaultFiles) / sizeof(defaultFiles[0]); ++i)
    {
        char source[128], destination[128], message[192];
        snprintf(source, sizeof(source), "app0:defaults/%s", defaultFiles[i]);
        snprintf(destination, sizeof(destination), "ux0:data/openyamm/%s", defaultFiles[i]);
        int result = oy_install_missing_file(source, destination);
        snprintf(message, sizeof(message), "FIRST RUN %s: %s", defaultFiles[i],
            result > 0 ? "created" : result == 0 ? "existing file preserved" : "copy failed");
        logLine(message);
        if (result < 0) defaultsReady = 0;
    }
    SceUID frameMemory = sceKernelAllocMemBlock("OpenYammBootstrap", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
        2 * 1024 * 1024, NULL);
    if (frameMemory >= 0 && sceKernelGetMemBlockBase(frameMemory, (void **)&pixels) >= 0)
    {
        memset(pixels, 0, 960 * 544 * 4);
        SceDisplayFrameBuf frame = {sizeof(frame), pixels, 960, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, 960, 544};
        int displayResult = sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
        if (displayResult < 0) logLine("FRAMEBUFFER SET FAILED");
    }
    showLine("OPENYAMM 01.06 - CLEAN BUILD");
    showLine("LOG: UX0:DATA/OPENYAMM-BOOT.LOG");
    // kubridge lives in the kernel, outside this application's module list.
    int searchBuffer[2] = {0};
    int bridgeResult = _vshKernelSearchModuleByName("kubridge", searchBuffer);
    showLine(bridgeResult >= 0 ? "KUBRIDGE: LOADED" : "KUBRIDGE: NOT DETECTED");
    char details[64];
    snprintf(details, sizeof(details), "KUBRIDGE RESULT: %08X", (unsigned int)bridgeResult);
    logLine(details);
    int shaderPresent = filePresent("ur0:data/libshacccg.suprx") ||
        filePresent("ur0:data/external/libshacccg.suprx");
    showLine(shaderPresent ? "SHADER COMPILER: FILE FOUND" : "SHADER COMPILER: FILE MISSING");
    showLine("FILE FOUND DOES NOT VERIFY COMPILER WORKS");
    int gamePresent = filePresent("app0:game.self") && filePresent("app0:libmain.so");
    showLine(gamePresent ? "GAME FILES: FOUND" : "GAME FILES: MISSING");
    const char *assetFiles[] = {"engine.zip", "worlds/mmmerge.zip", "worlds/mm6.zip",
        "worlds/mm7.zip", "worlds/mm8.zip"};
    int assetsReady = 1;
    const char *assetRoot = oy_asset_root(1, filePresent);
    char assetLocation[160];
    snprintf(assetLocation, sizeof(assetLocation), "DATA: %s", assetRoot);
    logLine(assetLocation);
    for (unsigned i = 0; i < sizeof(assetFiles) / sizeof(assetFiles[0]); ++i)
    {
        char path[128], message[128];
        snprintf(path, sizeof(path), "%s/%s", assetRoot, assetFiles[i]);
        if (!filePresent(path))
        {
            snprintf(message, sizeof(message), "MISSING: %s", assetFiles[i]);
            showLine(message);
            assetsReady = 0;
        }
    }
    if (!assetsReady) showLine("DATA GOES IN UX0:DATA/OPENYAMM/ASSETS");
    if (!defaultsReady) showLine("DEFAULT SETTINGS COPY FAILED - SEE LOG");
    int ready = bridgeResult >= 0 && shaderPresent && gamePresent && defaultsReady && assetsReady;
    showLine(ready ? "PRESS CROSS TO TEST GAME STARTUP" : "FIX MISSING ITEMS THEN RESTART THIS APP");
    showLine("X: START CLEAN BASELINE");
    showLine("PRESS CIRCLE TO EXIT");
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    for (;;)
    {
        SceCtrlData pad = {0};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_CIRCLE) break;
        if (ready && (pad.buttons & SCE_CTRL_CROSS))
        {
            showLine("STARTING GAME PROCESS");
            sceKernelDelayThread(200000);
            int result = sceAppMgrLoadExec("app0:game.self", NULL, NULL);
            snprintf(details, sizeof(details), "GAME START FAILED: %08X", (unsigned int)result);
            showLine(details);
            ready = 0;
        }
        sceKernelDelayThread(50000);
    }
    sceKernelExitProcess(0);
    return 0;
}
















