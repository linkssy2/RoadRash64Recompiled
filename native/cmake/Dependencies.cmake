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

# RR64-MAC: a working DXC for the Mac doing the build, for RT64's and
# RecompFrontend's shader rules (cmake/macos/DxcMac.cmake).
if(APPLE)
    include("${CMAKE_CURRENT_LIST_DIR}/macos/DxcMac.cmake")
endif()

# RR64-MAC: RT64's build-time tools (file_to_c, spirv_cross_msl) built for the
# build host, so a build for the other Mac architecture never runs its own
# target-architecture binaries (cmake/macos/HostTools.cmake). On by default on
# macOS; nothing else needs it, though it works there too.
if(APPLE)
    set(RR64_HOST_TOOLS_DEFAULT ON)
else()
    set(RR64_HOST_TOOLS_DEFAULT OFF)
endif()
option(RR64_HOST_TOOLS "Build RT64's file_to_c and spirv_cross_msl for the build host" ${RR64_HOST_TOOLS_DEFAULT})
if(RR64_HOST_TOOLS)
    include("${CMAKE_CURRENT_LIST_DIR}/macos/HostTools.cmake")
endif()

# RR64-SDL3: SDL3 3.4.16 (SDL3.dll on Windows, static on Linux), built before
# RT64 and RecompFrontend so all three compile against the same SDL3 headers
# and link one SDL3 library.
include("${CMAKE_CURRENT_LIST_DIR}/Sdl.cmake")

# Match the shared N64Recomp frontend stack used by current ports.
set(RT64_STATIC TRUE)
# RR64-MAC: macOS renders through RT64's Metal backend; the SDL window is a
# Metal window.
if(APPLE)
    set(RT64_SDL_WINDOW_VULKAN FALSE)
else()
    set(RT64_SDL_WINDOW_VULKAN TRUE)
endif()
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
    # RR64-MAC: the generated audio microcode (rr64_audio_rsp.cpp) uses
    # librecomp's RSP vector unit, which maps its SSE4.1 code onto NEON through
    # sse2neon on arm64.
    "${N64MODERN_RUNTIME_ROOT}/thirdparty/sse2neon"
)
# RR64-SIMD: the instruction floor (formerly -march=nehalem here) and
# -ffp-contract=off now come from cmake/Simd.cmake for every target.
if(MSVC)
    target_compile_options(RecompiledFuncs PRIVATE
        /clang:-fno-strict-aliasing
        /clang:-Wno-unused-variable
        /clang:-Wno-implicit-function-declaration
    )
else()
    target_compile_options(RecompiledFuncs PRIVATE
        -fno-strict-aliasing
        -Wno-unused-variable
        -Wno-implicit-function-declaration
    )
endif()

include(FetchContent)
# RR64-SDL3: the SDL2-devel download (Windows) and find_package(SDL2) (Linux)
# are replaced by cmake/Sdl.cmake, which sets RR64_SDL_* above.
if(NOT WIN32)
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
    set(RR64_FFMPEG_PLATFORM_ARGS "")
    if(APPLE)
        # RR64-MAC: macOS uses the same minimal decode-only FFmpeg as Linux
        # (WMA has no AudioToolbox decoder). The dylibs get @rpath install names
        # and are copied into the app bundle's Frameworks folder after linking.
        set(RR64_FFMPEG_SUFFIX ".dylib")
        set(RR64_FFMPEG_ARCH_FLAGS "-arch ${RR64_TARGET_ARCH} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        if(CMAKE_OSX_SYSROOT)
            # CMake adds the SDK only to its own compile lines; FFmpeg's
            # configure calls the compiler directly.
            string(APPEND RR64_FFMPEG_ARCH_FLAGS " -isysroot ${CMAKE_OSX_SYSROOT}")
        endif()
        list(APPEND RR64_FFMPEG_PLATFORM_ARGS
            "--cc=${CMAKE_C_COMPILER}"
            "--install-name-dir=@rpath"
            "--extra-cflags=${RR64_FFMPEG_ARCH_FLAGS}"
            "--extra-ldflags=${RR64_FFMPEG_ARCH_FLAGS}")
        string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" RR64_FFMPEG_HOST_ARCH)
        if(RR64_FFMPEG_HOST_ARCH MATCHES "^(arm64|aarch64)$")
            set(RR64_FFMPEG_HOST_ARCH "arm64")
        endif()
        if(NOT RR64_FFMPEG_HOST_ARCH STREQUAL RR64_TARGET_ARCH)
            # Building the Intel slice on Apple Silicon, or the reverse:
            # configure must not try to run the test programs it compiles.
            if(RR64_TARGET_ARCH STREQUAL "arm64")
                set(RR64_FFMPEG_TARGET_ARCH "aarch64")
            else()
                set(RR64_FFMPEG_TARGET_ARCH "x86_64")
            endif()
            list(APPEND RR64_FFMPEG_PLATFORM_ARGS "--enable-cross-compile" "--target-os=darwin" "--arch=${RR64_FFMPEG_TARGET_ARCH}")
        endif()
    else()
        set(RR64_FFMPEG_SUFFIX ".so")
    endif()
    set(RR64_FFMPEG_LIBS
        "${RR64_FFMPEG_PREFIX}/lib/libavformat${RR64_FFMPEG_SUFFIX}"
        "${RR64_FFMPEG_PREFIX}/lib/libavcodec${RR64_FFMPEG_SUFFIX}"
        "${RR64_FFMPEG_PREFIX}/lib/libswresample${RR64_FFMPEG_SUFFIX}"
        "${RR64_FFMPEG_PREFIX}/lib/libavutil${RR64_FFMPEG_SUFFIX}"
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
            ${RR64_FFMPEG_PLATFORM_ARGS}
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
    if(APPLE)
        # RR64-MAC: build-host DXC from cmake/macos/DxcMac.cmake (the Intel
        # binary repaired: its zlib reference pointed into a pkgx install).
        set(DXC ${RR64_DXC_MACOS})
    else()
        set(DXC "LD_LIBRARY_PATH=${RT64_ROOT}/src/contrib/dxc/lib/x64" "${RT64_ROOT}/src/contrib/dxc/bin/x64/dxc-linux")
    endif()
endif()

# RR64-MAC: FreeType for RmlUi. macOS has no system FreeType and Homebrew's is
# built for one architecture only, so on macOS the pinned FreeType 2.13.3
# (native/lib/freetype, fetched by setup_dependencies.py) is compiled here as a
# static library for this build's architecture, with none of its optional
# dependencies (zlib, bzip2, PNG, HarfBuzz, Brotli). RmlUi only checks that a
# Freetype::Freetype target exists, so the package search is switched off.
if(APPLE)
    set(RR64_BUNDLED_FREETYPE_DEFAULT ON)
else()
    set(RR64_BUNDLED_FREETYPE_DEFAULT OFF)
endif()
option(RR64_BUNDLED_FREETYPE "Build the pinned FreeType from native/lib/freetype for RmlUi" ${RR64_BUNDLED_FREETYPE_DEFAULT})
if(RR64_BUNDLED_FREETYPE)
    set(RR64_FREETYPE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/lib/freetype")
    if(NOT EXISTS "${RR64_FREETYPE_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR "FreeType source missing: ${RR64_FREETYPE_ROOT}\n"
            "Run scripts/setup_dependencies.py (it fetches FreeType on macOS).")
    endif()
    # FreeType's CMake predates policy CMP0077, so its options only read cache entries.
    foreach(RR64_FT_DEP ZLIB BZIP2 PNG HARFBUZZ BROTLI)
        set(FT_DISABLE_${RR64_FT_DEP} ON CACHE BOOL "Disabled for the bundled FreeType" FORCE)
    endforeach()
    set(SKIP_INSTALL_ALL ON)
    add_subdirectory("${RR64_FREETYPE_ROOT}" "${CMAKE_BINARY_DIR}/freetype" EXCLUDE_FROM_ALL)
    unset(SKIP_INSTALL_ALL)
    add_library(Freetype::Freetype ALIAS freetype)
    set(CMAKE_DISABLE_FIND_PACKAGE_Freetype TRUE)
    message(STATUS "RR64 FreeType: bundled 2.13.3 (static) from ${RR64_FREETYPE_ROOT}")
endif()

set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH "${N64MODERN_RUNTIME_ROOT}")
set(RECOMP_FRONTEND_RT64_PATH "${RT64_ROOT}")
add_subdirectory("${RECOMP_FRONTEND_ROOT}" "${CMAKE_BINARY_DIR}/RecompFrontend")
target_include_directories(recompui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(RT64_SDL_WINDOW_VULKAN)
    target_compile_definitions(recompui PUBLIC PLUME_SDL_VULKAN_ENABLED)
endif()
