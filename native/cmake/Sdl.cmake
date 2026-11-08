# RR64-SDL3: SDL3 3.4.16, built from source.
#
# The game, RT64 (window, imgui inspector, plume's Vulkan surface) and
# RecompFrontend (recompui, recompinput, the RmlUi SDL platform) are written
# directly against the SDL3 API; SDL2 is gone.
#
# How SDL3 is linked (RR64_SDL3_SHARED):
#   Windows  shared: SDL3.dll beside the EXE, the way the stock builds
#            shipped SDL2.dll, so a player can drop in a newer SDL3.dll
#            without a rebuild.
#            -DRR64_SDL3_SHARED=OFF links it into the EXE instead.
#   Linux    static: no libSDL*.so in the AppImage (SDL3 still dlopens the
#            system's X11/Wayland/PipeWire/PulseAudio/ALSA libraries)
# The DLL needs no copy step: ProjectOptions.cmake points every runtime output
# at <build>/bin before this file runs, so SDL3.dll is built straight into the
# folder that holds RoadRash64Recompiled.exe and the test EXEs.
#
# Either SDL3 target carries its own system link requirements (Windows:
# setupapi, winmm, imm32, version, cfgmgr32, ...), so linking RR64_SDL_TARGET
# is all a consumer needs.
#
# Entry point: nothing includes <SDL3/SDL_main.h>, so SDL never renames
# main(). The game keeps its own main() (and Windows' /ENTRY:mainCRTStartup in
# Release) and calls SDL_Init() directly; SDL_SetMainReady() lives in
# SDL_main.h and is not needed on desktop.

set(RR64_SDL3_VERSION "3.4.16")
set(RR64_SDL3_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/SDL3" CACHE PATH
    "SDL3 source tree (the pinned checkout, or an extracted SDL-release-${RR64_SDL3_VERSION}.zip)")

foreach(RR64_SDL_REQUIRED
        "${RR64_SDL3_SOURCE_DIR}/CMakeLists.txt"
        "${RR64_SDL3_SOURCE_DIR}/include/SDL3/SDL_version.h")
    if(NOT EXISTS "${RR64_SDL_REQUIRED}")
        message(FATAL_ERROR "SDL3 source missing: ${RR64_SDL_REQUIRED}\n"
            "Run scripts/setup_dependencies.py, or point "
            "-DRR64_SDL3_SOURCE_DIR at an extracted SDL-release-${RR64_SDL3_VERSION} folder.")
    endif()
endforeach()

# Read the version from the header so a mismatched source folder is caught at
# configure time. The code uses SDL 3.2 API (properties, SDL_Gamepad,
# SDL_AudioStream) and SDL_MouseWheelEvent.integer_y from 3.2.12, so any
# 3.2.12+ tree builds; 3.4.16 is the tested pin.
file(STRINGS "${RR64_SDL3_SOURCE_DIR}/include/SDL3/SDL_version.h" RR64_SDL3_VERSION_LINES
    REGEX "^#define SDL_(MAJOR|MINOR|MICRO)_VERSION[ \t]+[0-9]+")
foreach(RR64_PART MAJOR MINOR MICRO)
    string(REGEX MATCH "SDL_${RR64_PART}_VERSION[ \t]+([0-9]+)" _ "${RR64_SDL3_VERSION_LINES}")
    set(RR64_SDL3_${RR64_PART} "${CMAKE_MATCH_1}")
endforeach()
set(RR64_SDL3_FOUND_VERSION "${RR64_SDL3_MAJOR}.${RR64_SDL3_MINOR}.${RR64_SDL3_MICRO}")
if(NOT RR64_SDL3_MAJOR EQUAL 3 OR RR64_SDL3_FOUND_VERSION VERSION_LESS "3.2.12")
    message(FATAL_ERROR "SDL ${RR64_SDL3_FOUND_VERSION} in ${RR64_SDL3_SOURCE_DIR} is older than 3.2.12, the oldest SDL3 this code supports (SDL_MouseWheelEvent.integer_y).")
elseif(NOT RR64_SDL3_FOUND_VERSION VERSION_EQUAL RR64_SDL3_VERSION)
    message(WARNING "Building SDL3 ${RR64_SDL3_FOUND_VERSION}; this release is pinned to ${RR64_SDL3_VERSION}.")
endif()

if(WIN32)
    option(RR64_SDL3_SHARED "Build SDL3 as SDL3.dll beside the EXE (OFF links it statically)" ON)
else()
    # The AppImage is laid out for a static SDL3.
    set(RR64_SDL3_SHARED OFF)
endif()

# One SDL3 library, nothing installed, no tests or examples.
#   SDL_LIBC        normal C runtime, the same CRT as the rest of the build
#   SDL_TEST_LIBRARY  SDL3_test is not needed
if(RR64_SDL3_SHARED)
    set(SDL_SHARED ON)
    set(SDL_STATIC OFF)
else()
    set(SDL_SHARED OFF)
    set(SDL_STATIC ON)
endif()
set(SDL_TEST_LIBRARY OFF)
set(SDL_TESTS OFF)
set(SDL_EXAMPLES OFF)
set(SDL_INSTALL OFF)
set(SDL_DISABLE_UNINSTALL ON)
set(SDL_LIBC ON)
# RT64 creates its own Metal/D3D12/Vulkan devices; SDL's GPU and 2D renderer
# APIs are never used, so leave them out of the library.
set(SDL_GPU OFF)
set(SDL_RENDER OFF)
set(SDL_CAMERA OFF)
set(SDL_TRAY OFF)
add_subdirectory("${RR64_SDL3_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/SDL3" EXCLUDE_FROM_ALL)

if(RR64_SDL3_SHARED)
    set(RR64_SDL_TARGET SDL3::SDL3-shared)
    set(RR64_SDL_LINKAGE "shared, SDL3.dll")
else()
    set(RR64_SDL_TARGET SDL3::SDL3-static)
    set(RR64_SDL_LINKAGE "static")
endif()
if(NOT TARGET ${RR64_SDL_TARGET})
    message(FATAL_ERROR "The SDL3 tree did not define ${RR64_SDL_TARGET}.")
endif()

# Variables the rest of the build, RT64 and RecompFrontend read.
#   RR64_SDL_TARGET        link this; it brings the SDL3 headers and system libraries
#   SDL3_LIBRARIES         the same target, under the name RT64, plume and
#                          RecompFrontend read (dependency-patches)
#   RR64_SDL_INCLUDE_DIRS  headers only, for test targets that do not link SDL
set(RR64_SDL_INCLUDE_DIRS "$<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>")
set(SDL3_LIBRARIES ${RR64_SDL_TARGET})

message(STATUS "RR64 SDL: SDL3 ${RR64_SDL3_FOUND_VERSION} (${RR64_SDL_LINKAGE}) from ${RR64_SDL3_SOURCE_DIR}")
