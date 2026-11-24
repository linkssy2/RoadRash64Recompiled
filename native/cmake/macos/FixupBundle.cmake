# Post-build step for RoadRash64Recompiled.app (run with cmake -P).
#
# Inputs (-D):
#   BUNDLE          path to RoadRash64Recompiled.app
#   EXECUTABLE      name of the executable inside Contents/MacOS
#   SEARCH_DIRS     |-separated library folders (FFmpeg prefix, SDL3 output)
#   MAXPROT_SCRIPT  cmake/macos/set_text_maxprot.py
#   ENTITLEMENTS    cmake/macos/entitlements.plist
#   SIGN_IDENTITY   codesign identity ("-" for ad-hoc)
#
# 1. Every @rpath dylib the executable needs (libSDL3.0.dylib, the minimal
#    FFmpeg), and every @rpath dylib those need in turn, is copied from the
#    build folders into Contents/Frameworks under its install name. They are
#    all built with @rpath install names, and the executable's only rpath is
#    @executable_path/../Frameworks, so nothing needs rewriting. Any other
#    non-system dependency is an error: the app must run on Macs that have
#    none of these libraries installed.
# 2. __TEXT maxprot is raised for N64ModernRuntime code-mod patching.
# 3. The bundle is signed (ad-hoc by default) with the runtime entitlements.

cmake_minimum_required(VERSION 3.20)  # policies for this -P script (IN_LIST, ...)

set(APP_EXE "${BUNDLE}/Contents/MacOS/${EXECUTABLE}")
if(NOT EXISTS "${APP_EXE}")
    message(FATAL_ERROR "Bundle executable not found: ${APP_EXE}")
endif()

# The executable resolves bundled libraries through @rpath.
execute_process(COMMAND otool -l "${APP_EXE}" OUTPUT_VARIABLE RR64_LOAD_COMMANDS)
if(NOT RR64_LOAD_COMMANDS MATCHES "@executable_path/../Frameworks")
    execute_process(COMMAND install_name_tool -add_rpath "@executable_path/../Frameworks" "${APP_EXE}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()

# RR64-MAC: copy the @rpath libraries in. (CMake's fixup_bundle cannot
# resolve @rpath items while the only rpath points at the still-empty
# Frameworks folder.)
string(REPLACE "|" ";" RR64_SEARCH_DIRS "${SEARCH_DIRS}")
set(RR64_FRAMEWORKS "${BUNDLE}/Contents/Frameworks")
file(REMOVE_RECURSE "${RR64_FRAMEWORKS}")
file(MAKE_DIRECTORY "${RR64_FRAMEWORKS}")

function(rr64_linked_libraries BINARY OUT)
    execute_process(COMMAND otool -L "${BINARY}" OUTPUT_VARIABLE TEXT RESULT_VARIABLE RESULT)
    if(NOT RESULT EQUAL 0)
        message(FATAL_ERROR "otool -L failed on ${BINARY}")
    endif()
    string(REPLACE "\n" ";" LINES "${TEXT}")
    set(LIBS "")
    foreach(LINE IN LISTS LINES)
        # "<tab>@rpath/libSDL3.0.dylib (compatibility version ..., current version ...)"
        if(LINE MATCHES "^[ \t]+([^ \t].*) \\(compatibility version")
            list(APPEND LIBS "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set(${OUT} "${LIBS}" PARENT_SCOPE)
endfunction()

set(RR64_QUEUE "${APP_EXE}")
set(RR64_BUNDLED "")
while(RR64_QUEUE)
    list(POP_FRONT RR64_QUEUE RR64_BINARY)
    rr64_linked_libraries("${RR64_BINARY}" RR64_LIBS)
    foreach(RR64_LIB IN LISTS RR64_LIBS)
        if(RR64_LIB MATCHES "^(/usr/lib/|/System/)")
            continue()  # part of macOS
        elseif(RR64_LIB MATCHES "^@rpath/([^/]+)$")
            set(RR64_NAME "${CMAKE_MATCH_1}")
            if(RR64_NAME IN_LIST RR64_BUNDLED)
                continue()  # already copied (or a dylib's own install name)
            endif()
            set(RR64_SOURCE "")
            foreach(RR64_DIR IN LISTS RR64_SEARCH_DIRS)
                if(EXISTS "${RR64_DIR}/${RR64_NAME}")
                    set(RR64_SOURCE "${RR64_DIR}/${RR64_NAME}")
                    break()
                endif()
            endforeach()
            if(NOT RR64_SOURCE)
                message(FATAL_ERROR "${RR64_BINARY} needs @rpath/${RR64_NAME}, which is not in any of: ${RR64_SEARCH_DIRS}")
            endif()
            get_filename_component(RR64_REAL "${RR64_SOURCE}" REALPATH)
            set(RR64_DEST "${RR64_FRAMEWORKS}/${RR64_NAME}")
            execute_process(COMMAND cp -f "${RR64_REAL}" "${RR64_DEST}" COMMAND_ERROR_IS_FATAL ANY)
            execute_process(COMMAND chmod 644 "${RR64_DEST}" COMMAND_ERROR_IS_FATAL ANY)
            list(APPEND RR64_BUNDLED "${RR64_NAME}")
            list(APPEND RR64_QUEUE "${RR64_DEST}")
            message(STATUS "Bundled ${RR64_NAME}")
        else()
            message(FATAL_ERROR "${RR64_BINARY} links ${RR64_LIB}, which is neither part of macOS nor an @rpath "
                "library this build makes; the app would not run on other Macs.")
        endif()
    endforeach()
endwhile()

execute_process(COMMAND python3 "${MAXPROT_SCRIPT}" "${APP_EXE}" COMMAND_ERROR_IS_FATAL ANY)

# Sign nested code first, then the bundle. --deep is deprecated for signing.
file(GLOB RR64_NESTED "${BUNDLE}/Contents/Frameworks/*")
foreach(ITEM IN LISTS RR64_NESTED)
    if(NOT IS_SYMLINK "${ITEM}")
        execute_process(COMMAND codesign --force --sign "${SIGN_IDENTITY}" --timestamp=none "${ITEM}"
            COMMAND_ERROR_IS_FATAL ANY)
    endif()
endforeach()
execute_process(COMMAND codesign --force --sign "${SIGN_IDENTITY}" --timestamp=none --options runtime
    --entitlements "${ENTITLEMENTS}" "${BUNDLE}"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND codesign --verify --strict --verbose=2 "${BUNDLE}" COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "RoadRash64Recompiled.app bundled and signed (${SIGN_IDENTITY}).")
