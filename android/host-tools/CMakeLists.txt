# Host tools for the full-game Android build. Built on the HOST (x86_64)
# by scripts/build_android.py; the NDK build then uses them as imported
# executables (codegen + shader embedding run on the build machine, never
# on the phone).
cmake_minimum_required(VERSION 3.20)
project(rr64_android_host_tools LANGUAGES C CXX)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# N64Recomp translators (from the tools/ tree fetched by
# scripts/setup_dependencies.py).
add_subdirectory(../../tools/N64Recomp N64Recomp)
# file_to_c (from the RT64 tree): embeds compiled SPIR-V shaders as C
# arrays. RT64's Android patch skips building it for the target.
add_subdirectory(../../native/lib/rt64/src/tools/file_to_c file_to_c)
