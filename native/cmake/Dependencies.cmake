# Asset-free offline helper, built explicitly when packaging the local MK64
# importer. It executes bounded terrain queries without starting the game.
add_subdirectory("${ROOT_DIR}/tools/mk64-importer-contact"
    "${CMAKE_BINARY_DIR}/mk64-importer-contact")
add_subdirectory("${ROOT_DIR}/tools/mk64-importer-motion"
    "${CMAKE_BINARY_DIR}/mk64-importer-motion")

foreach(REQUIRED_PATH
    "${N64MODERN_RUNTIME_ROOT}/CMakeLists.txt"
    "${RT64_ROOT}/CMakeLists.txt"
    "${RECOMP_FRONTEND_ROOT}/CMakeLists.txt"
    "${RECOMPILED_DIR}/funcs.h"
    "${RECOMPILED_DIR}/funcs_0.c"
    "${RECOMPILED_DIR}/lookup.cpp"
    "${RECOMPILED_DIR}/recomp_overlays.inl"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/patch_helpers.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/recompui_event_structs.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/ui_funcs.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rr64_runtime_shims.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/NotoEmoji-Regular.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/recomp.rcss"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/RoadRashLauncher.svg"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/RoadRashLauncher-v4.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/achievement-toast-frame-v2.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/promptfont/promptfont.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/Caret.svg"
    "${RECOMP_FRONTEND_ROOT}/recompui/lib/RmlUi/Samples/assets/LatoLatin-Regular.ttf"
    "${RECOMP_FRONTEND_ROOT}/recompui/lib/RmlUi/Samples/assets/LICENSE.txt"
)
    if(NOT EXISTS "${REQUIRED_PATH}")
        message(FATAL_ERROR "Required native-stage input is missing: ${REQUIRED_PATH}")
    endif()
endforeach()

# Match the shared N64Recomp frontend stack used by current ports.
set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
set(BUILD_SHARED_LIBS OFF)
add_compile_definitions(HLSL_CPU NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

add_subdirectory("${RT64_ROOT}" "${CMAKE_BINARY_DIR}/rt64")
# Optional, local compiler profile for the frame matcher. Empty by default so
# source builds require no private training artifact. Keep this module-specific:
# an offline matcher profile does not represent simulation/audio/gameplay.
set(RR64_MATCHER_PROFILE "" CACHE FILEPATH "Optional Clang profile for RT64 frame matching")
if(RR64_MATCHER_PROFILE)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT EXISTS "${RR64_MATCHER_PROFILE}")
        message(FATAL_ERROR "RR64_MATCHER_PROFILE requires Clang and an existing profile file")
    endif()
    set_property(SOURCE "${RT64_ROOT}/src/hle/rt64_game_frame.cpp"
        TARGET_DIRECTORY rt64 APPEND PROPERTY COMPILE_OPTIONS
        "$<$<CONFIG:Release>:/clang:-fprofile-instr-use=${RR64_MATCHER_PROFILE}>")
endif()
add_subdirectory("${N64MODERN_RUNTIME_ROOT}" "${CMAKE_BINARY_DIR}/N64ModernRuntime")
target_include_directories(rt64 PRIVATE "${CMAKE_BINARY_DIR}/rt64/src")

add_library(RecompiledFuncs STATIC)
file(GLOB RR64_RECOMP_C CONFIGURE_DEPENDS "${RECOMPILED_DIR}/*.c")
file(GLOB RR64_RECOMP_CPP CONFIGURE_DEPENDS "${RECOMPILED_DIR}/*.cpp")
target_sources(RecompiledFuncs PRIVATE ${RR64_RECOMP_C} ${RR64_RECOMP_CPP})
target_include_directories(RecompiledFuncs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${N64MODERN_RUNTIME_ROOT}/ultramodern/include"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/include"
    "${N64MODERN_RUNTIME_ROOT}/N64Recomp/include"
)
if(MSVC)
    target_compile_options(RecompiledFuncs PRIVATE
        /clang:-march=nehalem
        /clang:-fno-strict-aliasing
        /clang:-Wno-unused-variable
        /clang:-Wno-implicit-function-declaration
    )
else()
    # Android/ARM64: -march=nehalem is x86-64-only and does not compile for
    # ARM64. The NDK toolchain already sets the right -march for the ABI.
    if(ANDROID)
        target_compile_options(RecompiledFuncs PRIVATE
            -fno-strict-aliasing
            -Wno-unused-variable
            -Wno-implicit-function-declaration
        )
    else()
        target_compile_options(RecompiledFuncs PRIVATE
            -march=nehalem
            -fno-strict-aliasing
            -Wno-unused-variable
            -Wno-implicit-function-declaration
        )
    endif()
endif()

include(FetchContent)
if(WIN32)
    set(SDL2_VERSION "2.30.3" CACHE STRING "SDL2 development package version")
    FetchContent_Declare(sdl2
        URL "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip")
    FetchContent_MakeAvailable(sdl2)
    set(RR64_SDL2_INCLUDE_DIRS "${sdl2_SOURCE_DIR}/include")
    set(RR64_SDL2_LIB_DIRS "${sdl2_SOURCE_DIR}/lib/x64")
    set(RR64_SDL2_TARGET SDL2)
elseif(ANDROID)
    # Android: SDL2 is built from source by cmake/Android.cmake (see
    # scripts/build_android.py); find_package(SDL2) cannot see it.
    if(NOT DEFINED RR64_ANDROID_SDL2_INCLUDE_DIRS)
        message(FATAL_ERROR "Android SDL2 include dir is not set: configure through cmake/Android.cmake (scripts/build_android.py)")
    endif()
    set(RR64_SDL2_INCLUDE_DIRS "${RR64_ANDROID_SDL2_INCLUDE_DIRS}")
    set(RR64_SDL2_LIB_DIRS "")
    set(RR64_SDL2_TARGET SDL2::SDL2)
    # No FFmpeg on Android v1: custom-soundtrack decoding is disabled (see
    # Application.cmake). Game audio itself does not need FFmpeg.
else()
    find_package(SDL2 2.26 REQUIRED)
    set(RR64_SDL2_INCLUDE_DIRS "${SDL2_INCLUDE_DIRS}")
    set(RR64_SDL2_LIB_DIRS "")
    set(RR64_SDL2_TARGET SDL2::SDL2)

    # Custom soundtrack support decodes FLAC/MP3/MP4/M4A/AAC/WMA through a
    # minimal, bundled shared FFmpeg built here from the pinned source in
    # native/lib/ffmpeg: audio decode only, no encoders, muxers, network,
    # video, or hardware acceleration. This mirrors how the Windows build
    # uses the OS-provided Media Foundation codecs for the same formats, but
    # avoids pulling in the huge dependency tree (200+ shared libraries,
    # including unrelated things like Samba and video encoders) that a
    # distro's full-featured FFmpeg package drags in.
    # Bundled shared libraries keep LGPL replacement straightforward. Exact
    # sources, configure flags and notices accompany the experimental download.
    include(ExternalProject)
    include(ProcessorCount)
    ProcessorCount(RR64_FFMPEG_NPROC)
    if(RR64_FFMPEG_NPROC EQUAL 0)
        set(RR64_FFMPEG_NPROC 1)
    endif()
    set(RR64_FFMPEG_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/lib/ffmpeg")
    set(RR64_FFMPEG_PREFIX "${CMAKE_BINARY_DIR}/ffmpeg-install")
    set(RR64_FFMPEG_INCLUDE_DIR "${RR64_FFMPEG_PREFIX}/include")
    set(RR64_FFMPEG_LIBS
        "${RR64_FFMPEG_PREFIX}/lib/libavformat.so"
        "${RR64_FFMPEG_PREFIX}/lib/libavcodec.so"
        "${RR64_FFMPEG_PREFIX}/lib/libswresample.so"
        "${RR64_FFMPEG_PREFIX}/lib/libavutil.so"
    )
    ExternalProject_Add(rr64_ffmpeg
        SOURCE_DIR "${RR64_FFMPEG_SOURCE}"
        BUILD_IN_SOURCE 0
        CONFIGURE_COMMAND "${RR64_FFMPEG_SOURCE}/configure"
            --disable-autodetect
            --disable-gpl
            --disable-nonfree
            --disable-everything
            --disable-programs
            --disable-doc
            --disable-avdevice
            --disable-avfilter
            --disable-swscale
            --disable-network
            --disable-encoders
            --disable-muxers
            --disable-bsfs
            --disable-hwaccels
            --disable-libdrm
            --disable-devices
            --disable-x86asm
            --disable-sdl2
            --disable-libxcb
            --disable-libxcb-shm
            --disable-libxcb-xfixes
            --disable-libxcb-shape
            --disable-iconv
            --disable-lzma
            --disable-bzlib
            --enable-decoder=mp3,mp3float,aac,flac,wmav1,wmav2
            --enable-demuxer=mp3,mov,flac,asf,aac
            --enable-parser=mpegaudio,aac,flac
            --enable-protocol=file
            --enable-swresample
            --disable-static
            --enable-shared
            --extra-cflags=-fPIC
            --prefix=${RR64_FFMPEG_PREFIX}
        BUILD_COMMAND make -j${RR64_FFMPEG_NPROC}
        INSTALL_COMMAND make install
        BUILD_BYPRODUCTS ${RR64_FFMPEG_LIBS}
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_INSTALL ON
        USES_TERMINAL_CONFIGURE ON
        USES_TERMINAL_BUILD ON
    )
endif()

# Voice chat uses Opus' low-latency speech mode. Build it statically with the
# port so release packages do not require another runtime DLL.
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    opus
    URL "https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz"
    URL_HASH "SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1"
)
FetchContent_MakeAvailable(opus)

# RecompFrontend's shader custom commands consume the DXC variables from the
# parent project. RT64 defines similar variables inside its own subdirectory,
# but those values do not propagate back to this scope.
if(MSVC)
    set(DXC_COMMON_OPTS "-I${CMAKE_CURRENT_SOURCE_DIR}/src")
    set(DXC_DXIL_OPTS "-Wno-ignored-attributes")
    set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
    set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T" "ps_6_0" "-D" "DYNAMIC_RENDER_PARAMS")
    set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T" "vs_6_0" "-D" "DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
    set(DXC_CS_OPTS "${DXC_COMMON_OPTS}" "-E" "CSMain" "-T" "cs_6_0")
    set(DXC_GS_OPTS "${DXC_COMMON_OPTS}" "-E" "GSMain" "-T" "gs_6_0")
    set(DXC_RT_OPTS "${DXC_COMMON_OPTS}" "-D" "RT_SHADER" "-T" "lib_6_3"
        "-fspv-target-env=vulkan1.1spirv1.4"
        "-fspv-extension=SPV_KHR_ray_tracing"
        "-fspv-extension=SPV_EXT_descriptor_indexing")
    set(DXC "${RT64_ROOT}/src/contrib/dxc/bin/x64/dxc.exe")
elseif(ANDROID)
    # Android cross-compile: the shader compiler must be the HOST dxc
    # binary (x86_64), never a target-arch one. scripts/build_android.py
    # sets RR64_HOST_DXC to it (same idea as AeroGauge's AERO_HOST_DXC).
    if(NOT DEFINED RR64_HOST_DXC)
        message(FATAL_ERROR "RR64_HOST_DXC is not set: configure through cmake/Android.cmake (scripts/build_android.py)")
    endif()
    set(DXC_COMMON_OPTS "-I${CMAKE_CURRENT_SOURCE_DIR}/src")
    set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
    set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T" "ps_6_0" "-D" "DYNAMIC_RENDER_PARAMS")
    set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T" "vs_6_0" "-D" "DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
    set(DXC_CS_OPTS "${DXC_COMMON_OPTS}" "-E" "CSMain" "-T" "cs_6_0")
    set(DXC_GS_OPTS "${DXC_COMMON_OPTS}" "-E" "GSMain" "-T" "gs_6_0")
    set(DXC_RT_OPTS "${DXC_COMMON_OPTS}" "-D" "RT_SHADER" "-T" "lib_6_3"
        "-fspv-target-env=vulkan1.1spirv1.4"
        "-fspv-extension=SPV_KHR_ray_tracing"
        "-fspv-extension=SPV_EXT_descriptor_indexing")
    set(DXC ${RR64_HOST_DXC})
else()
    set(DXC_COMMON_OPTS "-I${CMAKE_CURRENT_SOURCE_DIR}/src")
    set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
    set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T" "ps_6_0" "-D" "DYNAMIC_RENDER_PARAMS")
    set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T" "vs_6_0" "-D" "DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
    set(DXC_CS_OPTS "${DXC_COMMON_OPTS}" "-E" "CSMain" "-T" "cs_6_0")
    set(DXC_GS_OPTS "${DXC_COMMON_OPTS}" "-E" "GSMain" "-T" "gs_6_0")
    set(DXC_RT_OPTS "${DXC_COMMON_OPTS}" "-D" "RT_SHADER" "-T" "lib_6_3"
        "-fspv-target-env=vulkan1.1spirv1.4"
        "-fspv-extension=SPV_KHR_ray_tracing"
        "-fspv-extension=SPV_EXT_descriptor_indexing")
    set(DXC "LD_LIBRARY_PATH=${RT64_ROOT}/src/contrib/dxc/lib/x64" "${RT64_ROOT}/src/contrib/dxc/bin/x64/dxc-linux")
endif()

set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH "${N64MODERN_RUNTIME_ROOT}")
set(RECOMP_FRONTEND_RT64_PATH "${RT64_ROOT}")
add_subdirectory("${RECOMP_FRONTEND_ROOT}" "${CMAKE_BINARY_DIR}/RecompFrontend")
target_include_directories(recompui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(RT64_SDL_WINDOW_VULKAN)
    target_compile_definitions(recompui PUBLIC PLUME_SDL_VULKAN_ENABLED)
endif()
