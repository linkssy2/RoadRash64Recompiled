# RR64-MAC: build-time tools for the Mac doing the build (included from
# Dependencies.cmake before RT64, Apple only).
#
# RT64 turns every shader into C arrays during the build with two small tools
# of its own, file_to_c and spirv_cross_msl. Built inside this tree they come
# out for the target architecture, so an arm64 build on an Intel Mac could not
# run them at all, and an x86_64 build on Apple Silicon would run them under
# Rosetta with this build's AVX floor. Here they are built once more, at
# configure time, as a separate small project for the build host's own
# architecture and with no instruction floor; RT64 (through its dependency
# patch) runs these copies instead of building its own.
#
# Sets RT64_HOST_TOOLS_DIR, which RT64 reads. Every configure re-runs the
# small build, which does nothing when the tools are already up to date.

set(RR64_HOST_TOOLS_ROOT "${CMAKE_BINARY_DIR}/host-tools")
set(RR64_HOST_TOOLS_SOURCE "${RR64_HOST_TOOLS_ROOT}/src")
set(RR64_HOST_TOOLS_BUILD "${RR64_HOST_TOOLS_ROOT}/build")
set(RR64_HOST_TOOLS_BIN "${RR64_HOST_TOOLS_ROOT}/bin")

string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" RR64_HOST_TOOLS_ARCH)
if(RR64_HOST_TOOLS_ARCH MATCHES "^(arm64|aarch64)$")
    set(RR64_HOST_TOOLS_ARCH "arm64")
else()
    set(RR64_HOST_TOOLS_ARCH "x86_64")
endif()

file(MAKE_DIRECTORY "${RR64_HOST_TOOLS_SOURCE}")
file(CONFIGURE OUTPUT "${RR64_HOST_TOOLS_SOURCE}/CMakeLists.txt" CONTENT [=[
cmake_minimum_required(VERSION 3.20)
project(rr64_host_tools LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "@RR64_HOST_TOOLS_BIN@")
# Only the C++ library spirv_cross_msl links is needed from SPIRV-Cross.
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("@RT64_ROOT@/src/tools/file_to_c" file_to_c)
add_subdirectory("@RT64_ROOT@/src/tools/spirv_cross_msl" spirv_cross_msl)
]=] @ONLY)

if(CMAKE_GENERATOR MATCHES "Ninja|Unix Makefiles")
    set(RR64_HOST_TOOLS_GENERATOR "${CMAKE_GENERATOR}")
else()
    # An Xcode-generated parent still gets a plain command-line tools build.
    set(RR64_HOST_TOOLS_GENERATOR "Unix Makefiles")
endif()

if(NOT EXISTS "${RR64_HOST_TOOLS_BUILD}/CMakeCache.txt")
    message(STATUS "RR64 host tools: configuring file_to_c and spirv_cross_msl for ${RR64_HOST_TOOLS_ARCH}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${RR64_HOST_TOOLS_SOURCE}" -B "${RR64_HOST_TOOLS_BUILD}"
            -G "${RR64_HOST_TOOLS_GENERATOR}"
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_OSX_ARCHITECTURES=${RR64_HOST_TOOLS_ARCH}
            "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        RESULT_VARIABLE RR64_HOST_TOOLS_RESULT
        OUTPUT_VARIABLE RR64_HOST_TOOLS_OUTPUT
        ERROR_VARIABLE RR64_HOST_TOOLS_OUTPUT)
    if(NOT RR64_HOST_TOOLS_RESULT EQUAL 0)
        file(REMOVE_RECURSE "${RR64_HOST_TOOLS_BUILD}")
        message(FATAL_ERROR "Configuring the build-host tools failed:\n${RR64_HOST_TOOLS_OUTPUT}")
    endif()
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${RR64_HOST_TOOLS_BUILD}" --config Release --parallel
        --target file_to_c spirv_cross_msl
    RESULT_VARIABLE RR64_HOST_TOOLS_RESULT
    OUTPUT_VARIABLE RR64_HOST_TOOLS_OUTPUT
    ERROR_VARIABLE RR64_HOST_TOOLS_OUTPUT)
if(NOT RR64_HOST_TOOLS_RESULT EQUAL 0)
    message(FATAL_ERROR "Building the build-host tools failed:\n${RR64_HOST_TOOLS_OUTPUT}")
endif()
foreach(RR64_HOST_TOOL file_to_c spirv_cross_msl)
    if(NOT EXISTS "${RR64_HOST_TOOLS_BIN}/${RR64_HOST_TOOL}")
        message(FATAL_ERROR "Build-host tool missing after its build: ${RR64_HOST_TOOLS_BIN}/${RR64_HOST_TOOL}")
    endif()
endforeach()

set(RT64_HOST_TOOLS_DIR "${RR64_HOST_TOOLS_BIN}")
message(STATUS "RR64 host tools: ${RT64_HOST_TOOLS_DIR} (${RR64_HOST_TOOLS_ARCH})")
