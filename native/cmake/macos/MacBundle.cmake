# macOS application bundle for RoadRash64Recompiled (included from
# Application.cmake on Apple only).
#
#   RoadRash64Recompiled.app/Contents/
#     MacOS/RoadRash64Recompiled        the game
#     Resources/assets/                 frontend assets (RecompFrontend's
#                                       get_program_path() on Apple)
#     Resources/AppIcon.icns            generated from assets/RoadRashIcon.png
#     Frameworks/                       libSDL3.0.dylib (see Sdl.cmake), the
#                                       decode-only FFmpeg dylibs, and FreeType
#                                       if it came from a shared install
#                                       (build_mac.sh links it statically)
#
# Saves, settings, custom music and imported race packs live in
# ~/Library/Application Support/RoadRash64Recompiled (or beside the .app when a
# portable.txt sits next to it), never inside the signed bundle.

# The version comes from kVersion in main.cpp, so the Info.plist follows each
# release without a second edit.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp" RR64_VERSION_LINE
    REGEX "constexpr const char\\* kVersion = \"[0-9.]+\"" LIMIT_COUNT 1)
string(REGEX MATCH "[0-9]+(\\.[0-9]+)+" RR64_VERSION_FROM_SOURCE "${RR64_VERSION_LINE}")
if(NOT RR64_VERSION_FROM_SOURCE)
    message(FATAL_ERROR "Could not read kVersion from native/src/main.cpp")
endif()
set(RR64_VERSION "${RR64_VERSION_FROM_SOURCE}" CACHE STRING "Version written into the macOS Info.plist (default: kVersion in main.cpp)")
set(RR64_BUNDLE_IDENTIFIER "com.github.linkssy2.RoadRash64Recompiled" CACHE STRING "macOS bundle identifier")
set(RR64_CODESIGN_IDENTITY "-" CACHE STRING "codesign identity for the app bundle (- = ad-hoc)")

if(RR64_TARGET_ARCH STREQUAL "arm64")
    set(RR64_TARGET_ARCH_PLIST "arm64")
else()
    set(RR64_TARGET_ARCH_PLIST "x86_64")
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/Info.plist.in" "${CMAKE_BINARY_DIR}/Info.plist" @ONLY)

set(RR64_ICNS "${CMAKE_BINARY_DIR}/AppIcon.icns")
add_custom_command(OUTPUT "${RR64_ICNS}"
    COMMAND ${CMAKE_COMMAND} -D "SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/assets/RoadRashIcon.png"
        -D "OUTPUT=${RR64_ICNS}" -D "WORK=${CMAKE_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/MakeIcns.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/RoadRashIcon.png" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/MakeIcns.cmake"
    COMMENT "Generating AppIcon.icns"
    VERBATIM)
set_source_files_properties("${RR64_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
target_sources(RoadRash64Recompiled PRIVATE "${RR64_ICNS}")

set_target_properties(RoadRash64Recompiled PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/Info.plist"
    OUTPUT_NAME "RoadRash64Recompiled"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${RR64_CODESIGN_IDENTITY}"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/entitlements.plist"
    # Only the bundle's own Frameworks folder: no rpath back into the build
    # tree, so even on the building Mac the app loads the copies it carries.
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "@executable_path/../Frameworks")

# Build folders that hold the @rpath libraries the bundle needs (FFmpeg,
# SDL3). Passed '|'-separated: a ';' list does not survive the trip through
# the custom command into the -P script as separate entries.
set(RR64_BUNDLE_SEARCH_DIRS "${RR64_FFMPEG_PREFIX}/lib")
if(RR64_SDL3_SHARED)
    list(APPEND RR64_BUNDLE_SEARCH_DIRS "$<TARGET_FILE_DIR:SDL3::SDL3-shared>")
endif()
string(REPLACE ";" "|" RR64_BUNDLE_SEARCH_DIRS_ARG "${RR64_BUNDLE_SEARCH_DIRS}")

# Third-party notices travel with the bundle (SDL3, FFmpeg and FreeType are
# inside it). Copied before the fix-up step below signs the bundle.
add_custom_command(TARGET RoadRash64Recompiled POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ROOT_DIR}/licenses"
        "$<TARGET_BUNDLE_CONTENT_DIR:RoadRash64Recompiled>/Resources/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ROOT_DIR}/THIRD_PARTY_NOTICES.md"
        "$<TARGET_BUNDLE_CONTENT_DIR:RoadRash64Recompiled>/Resources/THIRD_PARTY_NOTICES.md"
    VERBATIM)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(TARGET RoadRash64Recompiled POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -D "BUNDLE=$<TARGET_BUNDLE_DIR:RoadRash64Recompiled>"
        -D "EXECUTABLE=RoadRash64Recompiled"
        -D "SEARCH_DIRS=${RR64_BUNDLE_SEARCH_DIRS_ARG}"
        -D "MAXPROT_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/set_text_maxprot.py"
        -D "ENTITLEMENTS=${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/entitlements.plist"
        -D "SIGN_IDENTITY=${RR64_CODESIGN_IDENTITY}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/FixupBundle.cmake"
    COMMENT "Bundling, patching and signing RoadRash64Recompiled.app"
    VERBATIM)

# RecompFrontend's support_apple.mm (bundle paths, main-thread dispatch for
# file dialogs) and nativefiledialog-extended's Cocoa backend use these.
target_link_libraries(RoadRash64Recompiled PRIVATE "-framework Foundation" "-framework AppKit" "-framework UniformTypeIdentifiers")
