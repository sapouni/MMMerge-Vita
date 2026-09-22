cmake_minimum_required(VERSION 3.20)
set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake")
project(openyamm_vita_loader C CXX ASM)
include("${VITASDK}/share/vita.cmake")

set(PORT_WORK "" CACHE PATH "Directory containing the pinned SDK, dependencies and build results")
set(PORT_VITAGL_LIBRARY "${PORT_WORK}/deps/vitagl-softfp/libvitaGL.a" CACHE FILEPATH "VitaGL library variant")
if(NOT EXISTS "${PORT_WORK}/build-sdl-vita-softfp/libSDL3.a")
    message(FATAL_ERROR "Build the Android-compatible Vita SDL3 library first and set PORT_WORK")
endif()
option(OPENYAMM_VITA_PROFILE "Enable native diagnostic wrappers and collection" OFF)
option(OPENYAMM_VITA_AGGREGATE "Aggregate transport without GL or allocation tracing" OFF)
if (OPENYAMM_VITA_AGGREGATE)
    set(OPENYAMM_VITA_PROFILE ON CACHE BOOL "Enable native diagnostic wrappers and collection" FORCE)
endif()
option(OPENYAMM_PROFILE_NO_AUDIO "Disable all audio in diagnostic guest" OFF)
option(OPENYAMM_PROFILE_UI_ONLY "Skip world drawing, retain simulation and UI" OFF)
option(OPENYAMM_VITA_SAMPLER "Build the separate PC sampling host" OFF)
set(CMAKE_C_STANDARD 11)
add_executable(openyamm-vita loader-main.c startup-memory.c bridge-symbols-generated.c bridge-jump.S
    "${PORT_WORK}/loader-deps/so_util/so_util.c")
if (OPENYAMM_VITA_SAMPLER)
    target_sources(openyamm-vita PRIVATE sampler/host.c)
    target_compile_definitions(openyamm-vita PRIVATE OY_PC_SAMPLER=1)
    target_compile_options(openyamm-vita PRIVATE -g)
endif()
target_compile_definitions(openyamm-vita PRIVATE
    OY_PROFILE_ENABLED=$<BOOL:${OPENYAMM_VITA_PROFILE}>
    OY_PROFILE_AGGREGATE=$<BOOL:${OPENYAMM_VITA_AGGREGATE}>
    OY_PROFILE_NO_AUDIO=$<BOOL:${OPENYAMM_PROFILE_NO_AUDIO}>
    OY_PROFILE_UI_ONLY=$<BOOL:${OPENYAMM_PROFILE_UI_ONLY}>)
target_compile_options(openyamm-vita PRIVATE -fno-short-enums -Wall -Wextra -Werror=implicit-function-declaration)
target_include_directories(openyamm-vita PRIVATE
    "${PORT_WORK}/deps/sdl3/include" "${PORT_WORK}/deps/vitagl-softfp/source"
    "${PORT_WORK}/loader-deps/so_util")
target_link_libraries(openyamm-vita PRIVATE
    "${PORT_WORK}/build-sdl-vita-softfp/libSDL3.a"
    "${PORT_VITAGL_LIBRARY}"
    stdc++ mathneon vitashark SceShaccCgExt taihen_stub kubridge_stub pthread
    SceShaccCg_stub SceKernelDmacMgr_stub SceLibKernel_stub
    SceGxm_stub SceDisplay_stub SceCtrl_stub SceAppMgr_stub SceAppUtil_stub
    SceAudio_stub SceAudioIn_stub SceSysmodule_stub SceIofilemgr_stub SceCommonDialog_stub
    SceTouch_stub SceHid_stub SceMotion_stub ScePower_stub SceProcessmgr_stub SceCamera_stub SceVshBridge_stub m)
target_link_options(openyamm-vita PRIVATE -Wl,--wrap=sceKernelCreateLwMutex -Wl,--wrap=sceKernelLockLwMutex -Wl,--wrap=sceKernelUnlockLwMutex -Wl,--wrap=sceKernelTryLockLwMutex -Wl,--wrap=__malloc_lock -Wl,--wrap=__malloc_unlock -Wl,--wrap=_sbrk_r)
vita_create_self(game.self openyamm-vita UNSAFE NOASLR)
add_executable(openyamm-bootstrap bootstrap.c)
target_link_libraries(openyamm-bootstrap PRIVATE SceVshBridge_stub SceDisplay_stub SceCtrl_stub SceAppMgr_stub
    SceIofilemgr_stub SceLibKernel_stub SceProcessmgr_stub)
vita_create_self(eboot.bin openyamm-bootstrap UNSAFE)

# Packaging is deliberately opt-in until the import bridge is complete.
option(OPENYAMM_PACKAGE_DEVELOPMENT_LOADER "Package an explicitly unfinished development loader" OFF)
if(OPENYAMM_PACKAGE_DEVELOPMENT_LOADER)
    vita_create_vpk(OpenYAMM-development.vpk OYAM00001 eboot.bin
        VERSION "00.01" NAME "OpenYAMM development"
        FILE "${PORT_WORK}/build-android/android/app/src/main/cpp/libmain.so" libmain.so)
endif()
