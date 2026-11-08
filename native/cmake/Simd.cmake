# RR64-SIMD: one instruction-set floor and one floating-point model for every
# target in the build (game, runtime, RT64, frontend, Opus, helpers).
#
# Why global: hlslpp, RT64 and the frontend share inline headers. Mixing ISA
# flags between libraries is an ODR hazard (hlslpp changes code paths per
# ISA), so the floor must be identical for every translation unit.
#
#   x86-64  AVX floor by default (Sandy Bridge / Ivy Bridge / Bulldozer and
#           newer, which covers every Intel Mac supported by macOS 11). AVX2
#           kernels are selected at run time (RT64::RR64Simd::level()).
#           -DRR64_X86_BASELINE=AVX2 raises the floor to x86-64-v3 for a
#           local build; that binary will refuse to start on AVX-only CPUs.
#   arm64   Apple Silicon (M1 and newer), NEON.
#
# Floating point: contraction of a*b+c into FMA is disabled everywhere.
# arm64 always has FMA and AppleClang contracts by default, which would make
# the recompiled game's float results differ from the x86 builds and from the
# N64 (online races require identical simulation on every peer). On x86 it
# keeps the AVX and AVX2 floors bit-identical for the game simulation.
#
# hlslpp is pinned to its 128-bit register layout (HLSLPP_NO_SIMD_REGISTER_256,
# see dependency-patches/native_lib_rt64_src_contrib_hlslpp.patch): AVX would
# otherwise change float4x4 alignment and math order across all of RT64.

set(RR64_X86_BASELINE "AVX" CACHE STRING "x86-64 instruction floor: AVX (default) or AVX2")
set_property(CACHE RR64_X86_BASELINE PROPERTY STRINGS AVX AVX2)

# Resolve the target architecture. On macOS CMAKE_OSX_ARCHITECTURES selects
# the target (cross builds from Apple Silicon to Intel and back); elsewhere
# the processor name does.
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES RR64_OSX_ARCH_COUNT)
    if(RR64_OSX_ARCH_COUNT GREATER 1)
        message(FATAL_ERROR "Build one architecture per build tree (the x86-64 and arm64 floors differ). "
            "Use -DCMAKE_OSX_ARCHITECTURES=arm64 or -DCMAKE_OSX_ARCHITECTURES=x86_64.")
    endif()
    set(RR64_TARGET_ARCH "${CMAKE_OSX_ARCHITECTURES}")
elseif(MSVC AND CMAKE_GENERATOR_PLATFORM)
    set(RR64_TARGET_ARCH "${CMAKE_GENERATOR_PLATFORM}")
else()
    set(RR64_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${RR64_TARGET_ARCH}" RR64_TARGET_ARCH)
if(RR64_TARGET_ARCH MATCHES "^(x86_64|amd64|x64)$")
    set(RR64_TARGET_ARCH "x86_64")
elseif(RR64_TARGET_ARCH MATCHES "^(arm64|aarch64)$")
    set(RR64_TARGET_ARCH "arm64")
else()
    message(FATAL_ERROR "Unsupported target architecture '${RR64_TARGET_ARCH}'. Road Rash 64 Recompiled builds for x86-64 and arm64.")
endif()

set(RR64_ISA_FLAGS "")
set(RR64_ISA_DESCRIPTION "")
# Flags that undo the floor for the one translation unit that must run on any
# x86-64 CPU (the startup check in rr64_cpu_check.cpp).
set(RR64_ISA_BASELINE_FLAGS "")

if(RR64_TARGET_ARCH STREQUAL "x86_64")
    if(NOT RR64_X86_BASELINE MATCHES "^(AVX|AVX2)$")
        message(FATAL_ERROR "RR64_X86_BASELINE must be AVX or AVX2 (got '${RR64_X86_BASELINE}').")
    endif()
    if(MSVC)
        # clang-cl (the ClangCL toolset) and cl.exe both take /arch.
        list(APPEND RR64_ISA_FLAGS "/arch:${RR64_X86_BASELINE}")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # Never pass -march to clang-cl: once -march is present, clang-cl
            # ignores every /arch, including the per-file /arch:AVX2 that Opus
            # puts on its AVX2/FMA kernels (they then fail to compile). Name the
            # x86-64-v2/v3 extras that /arch leaves out as feature flags instead.
            list(APPEND RR64_ISA_FLAGS "/clang:-mpopcnt")
            if(RR64_X86_BASELINE STREQUAL "AVX2")
                list(APPEND RR64_ISA_FLAGS "/clang:-mfma" "/clang:-mbmi" "/clang:-mbmi2"
                    "/clang:-mlzcnt" "/clang:-mmovbe" "/clang:-mf16c")
            endif()
            list(APPEND RR64_ISA_FLAGS "/clang:-ffp-contract=off")
            set(RR64_ISA_BASELINE_FLAGS "/clang:-mno-avx" "/clang:-mno-f16c" "/clang:-mno-bmi" "/clang:-mno-bmi2" "/clang:-mno-lzcnt" "/clang:-mno-movbe")
        else()
            # cl.exe: /fp:precise is the default and VS 2022 does not contract
            # under it. A later /arch wins, so the check TU can step down.
            list(APPEND RR64_ISA_FLAGS "/fp:precise")
            set(RR64_ISA_BASELINE_FLAGS "/arch:SSE2")
        endif()
    else()
        if(RR64_X86_BASELINE STREQUAL "AVX")
            list(APPEND RR64_ISA_FLAGS "-march=x86-64-v2" "-mavx")
        else()
            list(APPEND RR64_ISA_FLAGS "-march=x86-64-v3")
        endif()
        list(APPEND RR64_ISA_FLAGS "-ffp-contract=off")
        # -mno-avx also removes every feature that depends on it (AVX2, FMA, F16C).
        set(RR64_ISA_BASELINE_FLAGS "-mno-avx" "-mno-bmi" "-mno-bmi2" "-mno-lzcnt" "-mno-movbe")
    endif()
    if(RR64_X86_BASELINE STREQUAL "AVX2")
        set(RR64_ISA_DESCRIPTION "x86-64 AVX2 floor (x86-64-v3)")
        add_compile_definitions(RR64_BASELINE_AVX2=1)
    else()
        set(RR64_ISA_DESCRIPTION "x86-64 AVX floor, AVX2 dispatched at run time")
    endif()
elseif(RR64_TARGET_ARCH STREQUAL "arm64")
    if(MSVC)
        message(FATAL_ERROR "Windows on ARM is not a supported target.")
    endif()
    if(APPLE)
        list(APPEND RR64_ISA_FLAGS "-mcpu=apple-m1")
        set(RR64_ISA_DESCRIPTION "arm64 Apple M1 baseline, NEON")
    else()
        set(RR64_ISA_DESCRIPTION "arm64 ARMv8-A, NEON")
    endif()
    list(APPEND RR64_ISA_FLAGS "-ffp-contract=off")
endif()

add_compile_options(${RR64_ISA_FLAGS})
add_compile_definitions(HLSLPP_NO_SIMD_REGISTER_256)
message(STATUS "RR64 instruction floor: ${RR64_ISA_DESCRIPTION} [${RR64_ISA_FLAGS}]")

# Attach the startup floor check to an executable. The source is compiled at
# the generic x86-64 level and runs before any other static initializer.
function(rr64_add_cpu_floor_check target)
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/rr64_cpu_check.cpp")
    if(RR64_ISA_BASELINE_FLAGS)
        set_source_files_properties("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/rr64_cpu_check.cpp"
            TARGET_DIRECTORY ${target}
            PROPERTIES COMPILE_OPTIONS "${RR64_ISA_BASELINE_FLAGS}")
    endif()
    if(WIN32)
        target_link_libraries(${target} PRIVATE User32.lib)
    endif()
endfunction()
