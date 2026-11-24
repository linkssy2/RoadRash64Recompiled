# RR64-MAC: a runnable DXC for the Mac doing the build (included from
# Dependencies.cmake before RT64 and RecompFrontend, Apple only).
#
# Sets RR64_DXC_MACOS to the command list RT64 (via its dependency patch) and
# RecompFrontend use as ${DXC}.
#
# Why: RT64's Intel macOS DXC (contrib/dxc, v1.8.2403.2) came out of a pkgx
# build. Both dxc-macos and libdxcompiler.dylib load zlib as
# "@rpath/zlib.net/v1/lib/libz.dylib", a path that only exists inside pkgx, and
# dxc-macos looks for libdxcompiler at "@loader_path/../lib", which is not
# where contrib keeps it. Upstream papered over this with
# DYLD_LIBRARY_PATH=<find_library(z) folder>, but with the Command Line Tools
# that folder is the SDK, which holds only libz.tbd (a link stub), so every
# shader compile aborts in dyld on an Intel Mac:
#   Library not loaded: @rpath/zlib.net/v1/lib/libz.dylib
#
# Fix: copy the two Intel files into <build>/dxc-macos-x64/{bin,lib} (so
# @loader_path/../lib finds libdxcompiler), point their zlib reference at the
# system's /usr/lib/libz.1.dylib (what the Apple Silicon DXC already uses; it
# is in every macOS's dyld shared cache), and re-sign them ad hoc, since
# install_name_tool invalidates the signature. The originals in contrib are not
# touched. Done again only when the contrib files change.
#
# The Apple Silicon DXC is unchanged (it already links /usr/lib/libz.1.dylib);
# it only needs its library folder on DYLD_LIBRARY_PATH.
#
# DXC is a build-host tool: the binary is chosen by the Mac doing the build,
# not by CMAKE_OSX_ARCHITECTURES, so Intel builds on Apple Silicon (and the
# reverse) run the native compiler.

set(RR64_DXC_CONTRIB "${RT64_ROOT}/src/contrib/dxc")
string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" RR64_DXC_HOST)

if(RR64_DXC_HOST MATCHES "^(arm64|aarch64)$")
    set(RR64_DXC_MACOS
        "DYLD_LIBRARY_PATH=${RR64_DXC_CONTRIB}/lib/arm64"
        "${RR64_DXC_CONTRIB}/bin/arm64/dxc-macos")
    message(STATUS "RR64 DXC: Apple Silicon dxc-macos from contrib")
    return()
endif()

set(RR64_DXC_X64_DIR "${CMAKE_BINARY_DIR}/dxc-macos-x64")
set(RR64_DXC_X64_SOURCES
    "${RR64_DXC_CONTRIB}/bin/x64/dxc-macos"
    "${RR64_DXC_CONTRIB}/lib/x64/libdxcompiler.dylib")
set(RR64_DXC_X64_OUTPUTS
    "${RR64_DXC_X64_DIR}/bin/dxc-macos"
    "${RR64_DXC_X64_DIR}/lib/libdxcompiler.dylib")
set(RR64_DXC_X64_STAMP "${RR64_DXC_X64_DIR}/fixed.stamp")
set(RR64_DXC_BAD_ZLIB "@rpath/zlib.net/v1/lib/libz.dylib")

set(RR64_DXC_X64_STALE FALSE)
if(NOT EXISTS "${RR64_DXC_X64_STAMP}")
    set(RR64_DXC_X64_STALE TRUE)
else()
    foreach(RR64_DXC_SOURCE IN LISTS RR64_DXC_X64_SOURCES)
        if("${RR64_DXC_SOURCE}" IS_NEWER_THAN "${RR64_DXC_X64_STAMP}")
            set(RR64_DXC_X64_STALE TRUE)
        endif()
    endforeach()
endif()

if(RR64_DXC_X64_STALE)
    file(REMOVE_RECURSE "${RR64_DXC_X64_DIR}")
    file(MAKE_DIRECTORY "${RR64_DXC_X64_DIR}/bin" "${RR64_DXC_X64_DIR}/lib")
    foreach(RR64_DXC_SOURCE RR64_DXC_OUTPUT IN ZIP_LISTS RR64_DXC_X64_SOURCES RR64_DXC_X64_OUTPUTS)
        if(NOT EXISTS "${RR64_DXC_SOURCE}")
            message(FATAL_ERROR "DXC file missing: ${RR64_DXC_SOURCE} (run scripts/setup_dependencies.py)")
        endif()
        # COPYONLY keeps the execute bit.
        configure_file("${RR64_DXC_SOURCE}" "${RR64_DXC_OUTPUT}" COPYONLY)

        # Only rewrite the reference if this copy actually has it, so a fixed
        # upstream binary passes through untouched.
        execute_process(COMMAND otool -L "${RR64_DXC_OUTPUT}" OUTPUT_VARIABLE RR64_DXC_LINKS COMMAND_ERROR_IS_FATAL ANY)
        if(RR64_DXC_LINKS MATCHES "zlib\\.net/v1/lib/libz\\.dylib")
            execute_process(COMMAND install_name_tool -change "${RR64_DXC_BAD_ZLIB}" "/usr/lib/libz.1.dylib" "${RR64_DXC_OUTPUT}"
                COMMAND_ERROR_IS_FATAL ANY)
        endif()
        execute_process(COMMAND codesign --force --sign - "${RR64_DXC_OUTPUT}" COMMAND_ERROR_IS_FATAL ANY)
    endforeach()

    # Prove it runs before any shader rule depends on it.
    execute_process(COMMAND "${RR64_DXC_X64_DIR}/bin/dxc-macos" -help
        RESULT_VARIABLE RR64_DXC_RESULT OUTPUT_QUIET ERROR_VARIABLE RR64_DXC_ERROR)
    if(NOT RR64_DXC_RESULT EQUAL 0)
        message(FATAL_ERROR "The fixed Intel dxc-macos does not run (${RR64_DXC_RESULT}):\n${RR64_DXC_ERROR}")
    endif()
    file(WRITE "${RR64_DXC_X64_STAMP}" "zlib -> /usr/lib/libz.1.dylib, ad-hoc signed\n")
    message(STATUS "RR64 DXC: Intel dxc-macos fixed up in ${RR64_DXC_X64_DIR}")
endif()

set(RR64_DXC_MACOS
    "DYLD_LIBRARY_PATH=${RR64_DXC_X64_DIR}/lib"
    "${RR64_DXC_X64_DIR}/bin/dxc-macos")
