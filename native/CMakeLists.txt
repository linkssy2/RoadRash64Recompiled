cmake_minimum_required(VERSION 3.20)
project(RoadRash64Recompiled LANGUAGES C CXX)

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
