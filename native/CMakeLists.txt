cmake_minimum_required(VERSION 3.20)
if(CMAKE_HOST_APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # RR64-MAC: Apple Silicon only, macOS 13 Ventura or newer. Every Apple
    # Silicon Mac can run Ventura, and from Ventura on all of their GPUs are in
    # the Metal 3 family, so RT64 always takes one path (Tier 2 argument
    # buffers with resource IDs and GPU buffer addresses). Both must be set
    # before project(), where APPLE is not defined yet. An Intel Mac can build
    # this (cross-compiling); the app itself is arm64.
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architecture (macOS builds are arm64 only)")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()
project(RoadRash64Recompiled LANGUAGES C CXX)
if(APPLE)
    # RecompFrontend's support_apple.mm and nativefiledialog's Cocoa backend.
    enable_language(OBJC OBJCXX)
    if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
        message(FATAL_ERROR "The macOS build targets Apple Silicon only (CMAKE_OSX_ARCHITECTURES=arm64, got "
            "'${CMAKE_OSX_ARCHITECTURES}'). Intel Macs are not supported for now: RT64 needs Metal argument "
            "buffers, which older AMD GPU drivers cannot compile.")
    endif()
    if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "13.0")
        message(FATAL_ERROR "The macOS build targets macOS 13 Ventura or newer (got "
            "${CMAKE_OSX_DEPLOYMENT_TARGET}); use -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 or newer.")
    endif()
    # Xcode 16's libc++ (before libc++ 20) keeps std::jthread/std::stop_token
    # behind its experimental switch. Both are header-only there, so this
    # needs no extra library at link time.
    add_compile_definitions($<$<COMPILE_LANGUAGE:CXX,OBJCXX>:_LIBCPP_ENABLE_EXPERIMENTAL>)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-std=c++20")
    set(CMAKE_REQUIRED_DEFINITIONS "-D_LIBCPP_ENABLE_EXPERIMENTAL")
    check_cxx_source_compiles([=[
        #include <atomic>
        #include <filesystem>
        #include <stop_token>
        #include <thread>
        int main() {
            std::atomic<int> a{0}; a.notify_all();
            std::jthread t([](std::stop_token s) { (void)s.stop_requested(); });
            return std::filesystem::path("x").empty() ? 1 : 0;
        }
    ]=] RR64_HAVE_JTHREAD)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_DEFINITIONS)
    if(NOT RR64_HAVE_JTHREAD)
        message(FATAL_ERROR "This Xcode's C++ library has no std::jthread. Install a newer Xcode "
            "(16.3 or later), select it with xcode-select, then delete the build folder and configure again.")
    endif()
    message(STATUS "RR64 macOS: arm64, minimum macOS ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

# Keep the entry point a build-order map; each module owns one responsibility.
# Simd.cmake comes first: the instruction floor and FP model are global.
include(cmake/Simd.cmake)
include(cmake/ProjectOptions.cmake)
include(cmake/Dependencies.cmake)
include(cmake/ReplayWorkers.cmake)
include(cmake/Application.cmake)
include(cmake/SimdHarness.cmake)

# ON preserves existing verification scripts. Ordinary source builds can omit
# offline fixture generation with -DRR64_BUILD_TESTS=OFF.
option(RR64_BUILD_TESTS "Configure the ROM-free/offline regression targets" ON)
if(RR64_BUILD_TESTS)
    include(cmake/OfflineTests.cmake)
endif()
