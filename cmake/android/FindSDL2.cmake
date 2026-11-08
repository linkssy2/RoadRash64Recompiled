# All consumers share the SDL built for this ABI.
# (Adapted from AeroGauge's cmake/android/FindSDL2.cmake: AERO_ANDROID_DEPS
# -> RR64_ANDROID_DEPS, which cmake/Android.cmake sets.)
if(NOT TARGET SDL2::SDL2)
    message(FATAL_ERROR "Android SDL must be built before configuring the renderer")
endif()
set(SDL2_FOUND TRUE)
set(SDL2_INCLUDE_DIRS "${RR64_ANDROID_DEPS}/SDL/include")
set(SDL2_LIBRARIES SDL2::SDL2)
