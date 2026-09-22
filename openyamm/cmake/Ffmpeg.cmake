include_guard(GLOBAL)
include(AndroidArchitecture)

function(openyamm_configure_ffmpeg)
    if (TARGET openyamm_ffmpeg_build)
        return()
    endif()

    find_program(OPENYAMM_FFMPEG_MAKE_PROGRAM make REQUIRED)

    if (NOT DEFINED OPENYAMM_FFMPEG_SOURCE_DIR OR OPENYAMM_FFMPEG_SOURCE_DIR STREQUAL "")
        FetchContent_GetProperties(ffmpeg SOURCE_DIR OPENYAMM_FFMPEG_SOURCE_DIR)
    endif()

    set(OPENYAMM_FFMPEG_BUILD_DIR "${CMAKE_BINARY_DIR}/_deps/ffmpeg-build")
    file(MAKE_DIRECTORY "${OPENYAMM_FFMPEG_BUILD_DIR}")
    set(OPENYAMM_FFMPEG_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/ffmpeg-install")
    set(OPENYAMM_FFMPEG_INCLUDE_DIR "${OPENYAMM_FFMPEG_INSTALL_DIR}/include")
    set(OPENYAMM_FFMPEG_LIBAVFORMAT "${OPENYAMM_FFMPEG_INSTALL_DIR}/lib/libavformat.a")
    set(OPENYAMM_FFMPEG_LIBAVCODEC "${OPENYAMM_FFMPEG_INSTALL_DIR}/lib/libavcodec.a")
    set(OPENYAMM_FFMPEG_LIBSWRESAMPLE "${OPENYAMM_FFMPEG_INSTALL_DIR}/lib/libswresample.a")
    set(OPENYAMM_FFMPEG_LIBSWSCALE "${OPENYAMM_FFMPEG_INSTALL_DIR}/lib/libswscale.a")
    set(OPENYAMM_FFMPEG_LIBAVUTIL "${OPENYAMM_FFMPEG_INSTALL_DIR}/lib/libavutil.a")

    set(openyammFfmpegInstallPrefix "${OPENYAMM_FFMPEG_INSTALL_DIR}")
    if (CMAKE_HOST_WIN32)
        # Keep MSYS install operations within the build tree, without drive prefixes.
        file(RELATIVE_PATH openyammFfmpegInstallPrefix
            "${OPENYAMM_FFMPEG_BUILD_DIR}" "${OPENYAMM_FFMPEG_INSTALL_DIR}")
    endif()

    set(openyammFfmpegConfigureArgs
        --prefix=${openyammFfmpegInstallPrefix}
        --disable-programs
        --disable-doc
        --disable-network
        --disable-autodetect
        --disable-everything
        --disable-asm
        --disable-avdevice
        --disable-avfilter
        --disable-iconv
        --enable-static
        --disable-shared
        --enable-avcodec
        --enable-avformat
        --enable-avutil
        --enable-swresample
        --enable-swscale
        --enable-protocol=file
        --enable-demuxer=ogg
        --enable-demuxer=mp3
        --enable-demuxer=wav
        --enable-decoder=theora
        --enable-decoder=vorbis
        --enable-decoder=mp3
        --enable-decoder=mp3float
        --enable-decoder=adpcm_ima_wav
        --enable-parser=vorbis
        --enable-parser=mpegaudio
    )

    if (ANDROID)
        openyamm_android_ffmpeg_architecture(
            "${CMAKE_ANDROID_ARCH_ABI}" openyammFfmpegArch openyammFfmpegCompilerPrefix)

        set(openyammFfmpegAndroidApiLevel "${CMAKE_SYSTEM_VERSION}")
        if (DEFINED ANDROID_PLATFORM AND ANDROID_PLATFORM MATCHES "^android-([0-9]+)$")
            set(openyammFfmpegAndroidApiLevel "${CMAKE_MATCH_1}")
        endif()

        get_filename_component(openyammAndroidToolchainBinDir "${CMAKE_AR}" DIRECTORY)
        set(openyammFfmpegCc
            "${openyammAndroidToolchainBinDir}/${openyammFfmpegCompilerPrefix}${openyammFfmpegAndroidApiLevel}-clang")
        if (CMAKE_HOST_WIN32)
            set(openyammFfmpegCc "${openyammAndroidToolchainBinDir}/clang.exe")
            set(openyammFfmpegTarget "${openyammFfmpegCompilerPrefix}${openyammFfmpegAndroidApiLevel}")
            list(APPEND openyammFfmpegConfigureArgs
                "--extra-cflags=--target=${openyammFfmpegTarget}"
                "--extra-ldflags=--target=${openyammFfmpegTarget}")
        endif()

        if (NOT EXISTS "${openyammFfmpegCc}")
            message(FATAL_ERROR "Android FFmpeg compiler not found: ${openyammFfmpegCc}")
        endif()

        list(APPEND openyammFfmpegConfigureArgs
            --enable-cross-compile
            --target-os=android
            --arch=${openyammFfmpegArch}
            --cc=${openyammFfmpegCc}
            --ar=${CMAKE_AR}
            --ranlib=${CMAKE_RANLIB}
            --strip=${CMAKE_STRIP}
            --nm=${CMAKE_NM}
        )

        if (NOT CMAKE_SYSROOT STREQUAL "")
            list(APPEND openyammFfmpegConfigureArgs --sysroot=${CMAKE_SYSROOT})
        endif()
    endif()

    add_custom_command(
        OUTPUT
            "${OPENYAMM_FFMPEG_LIBAVFORMAT}"
            "${OPENYAMM_FFMPEG_LIBAVCODEC}"
            "${OPENYAMM_FFMPEG_LIBSWRESAMPLE}"
            "${OPENYAMM_FFMPEG_LIBSWSCALE}"
            "${OPENYAMM_FFMPEG_LIBAVUTIL}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OPENYAMM_FFMPEG_BUILD_DIR}"
        COMMAND bash "${OPENYAMM_FFMPEG_SOURCE_DIR}/configure"
            ${openyammFfmpegConfigureArgs}
        COMMAND ${OPENYAMM_FFMPEG_MAKE_PROGRAM} -j4
        COMMAND ${OPENYAMM_FFMPEG_MAKE_PROGRAM} install-libs install-headers
        WORKING_DIRECTORY "${OPENYAMM_FFMPEG_BUILD_DIR}"
        DEPENDS
            "${OPENYAMM_FFMPEG_SOURCE_DIR}/configure"
            "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        COMMENT "Building bundled FFmpeg static libraries for house video playback"
        VERBATIM
    )

    add_custom_target(openyamm_ffmpeg_build
        DEPENDS
            "${OPENYAMM_FFMPEG_LIBAVFORMAT}"
            "${OPENYAMM_FFMPEG_LIBAVCODEC}"
            "${OPENYAMM_FFMPEG_LIBSWRESAMPLE}"
            "${OPENYAMM_FFMPEG_LIBSWSCALE}"
            "${OPENYAMM_FFMPEG_LIBAVUTIL}"
    )

    set(OPENYAMM_FFMPEG_INCLUDE_DIR "${OPENYAMM_FFMPEG_INCLUDE_DIR}" PARENT_SCOPE)
    set(OPENYAMM_FFMPEG_LIBAVFORMAT "${OPENYAMM_FFMPEG_LIBAVFORMAT}" PARENT_SCOPE)
    set(OPENYAMM_FFMPEG_LIBAVCODEC "${OPENYAMM_FFMPEG_LIBAVCODEC}" PARENT_SCOPE)
    set(OPENYAMM_FFMPEG_LIBSWRESAMPLE "${OPENYAMM_FFMPEG_LIBSWRESAMPLE}" PARENT_SCOPE)
    set(OPENYAMM_FFMPEG_LIBSWSCALE "${OPENYAMM_FFMPEG_LIBSWSCALE}" PARENT_SCOPE)
    set(OPENYAMM_FFMPEG_LIBAVUTIL "${OPENYAMM_FFMPEG_LIBAVUTIL}" PARENT_SCOPE)
endfunction()

function(openyamm_target_link_ffmpeg targetName)
    target_include_directories(${targetName}
        PRIVATE
            "${OPENYAMM_FFMPEG_INCLUDE_DIR}"
    )

    target_link_libraries(${targetName}
        PRIVATE
            "${OPENYAMM_FFMPEG_LIBAVFORMAT}"
            "${OPENYAMM_FFMPEG_LIBAVCODEC}"
            "${OPENYAMM_FFMPEG_LIBSWRESAMPLE}"
            "${OPENYAMM_FFMPEG_LIBSWSCALE}"
            "${OPENYAMM_FFMPEG_LIBAVUTIL}"
    )

    if (WIN32)
        target_link_libraries(${targetName} PRIVATE bcrypt)
    endif()

    add_dependencies(${targetName} openyamm_ffmpeg_build)
endfunction()
