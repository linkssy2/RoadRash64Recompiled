# Host tools for the probe. Only file_to_c: the probe needs no
# N64Recomp/RSPRecomp (no codegen, no ROM). Built on the HOST (x86_64) by
# scripts/build_probe.py; RT64's Android patch skips building file_to_c for
# the target and the probe CMakeLists imports this host binary instead.
cmake_minimum_required(VERSION 3.20)
project(rr64_probe_host_tools LANGUAGES C CXX)
if(NOT DEFINED RR64_RT64_DIR)
    message(FATAL_ERROR "RR64_RT64_DIR is not set (run scripts/build_probe.py)")
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_subdirectory(${RR64_RT64_DIR}/src/tools/file_to_c file_to_c)
