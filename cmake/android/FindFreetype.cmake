# FreeType for RmlUi (RecompFrontend). Built from source by
# cmake/Android.cmake before RecompFrontend configures.
# (Verbatim from AeroGauge's cmake/android/FindFreetype.cmake.)
if(NOT TARGET Freetype::Freetype)
    message(FATAL_ERROR "Android FreeType must be built before RmlUi")
endif()
set(FREETYPE_FOUND TRUE)
