$ErrorActionPreference = 'Stop'
$portRoot = (Resolve-Path work).Path.Replace('\', '/')
$env:PATH = "$portRoot/toolchains/msys64/ucrt64/bin;$portRoot/toolchains/msys64/usr/bin;$env:PATH"
$portArgs = @('-S', "$portRoot/openyamm", '-B', "$portRoot/build-android", '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$portRoot/build-tools-resumed/bin/ninja.exe",
    "-DCMAKE_TOOLCHAIN_FILE=$portRoot/toolchains/android-ndk-r28c/build/cmake/android.toolchain.cmake",
    '-DANDROID_ABI=armeabi-v7a', '-DANDROID_PLATFORM=android-23', '-DANDROID_STL=c++_static',
    '-DCMAKE_BUILD_TYPE=Release', '-DOPENYAMM_BUILD_TESTS=OFF', '-DOPENYAMM_BUILD_EDITOR=OFF',
    '-DOPENYAMM_BUILD_TOOLS=OFF', '-DOPENYAMM_BUILD_DESKTOP_EXECUTABLE=OFF',
    '-DOPENYAMM_VITA_LOADER=ON', '-DOPENYAMM_WINDOW_WIDTH=960', '-DOPENYAMM_WINDOW_HEIGHT=544',
    '-DOPENYAMM_DEV_ASSETS_DIR=assets', "-DOPENYAMM_HOST_SHADERC=$portRoot/build-host/openyamm_shaderc.exe")
foreach ($portDep in @('sdl3','fmt','spdlog','freetype','physfs','yaml-cpp','imgui','bx','bimg','bgfx','ffmpeg','lua')) {
    $portArgs += "-DFETCHCONTENT_SOURCE_DIR_$($portDep.ToUpper())=$portRoot/deps/$portDep"
}
& "$portRoot/build-tools-resumed/cmake/data/bin/cmake.exe" @portArgs
exit $LASTEXITCODE

