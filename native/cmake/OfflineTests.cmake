# Ordered because later fixtures reuse the earlier targets' include paths.
# Every executable remains EXCLUDE_FROM_ALL: configure never runs a game.
include(cmake/tests/NetworkAndReplay.cmake)
include(cmake/tests/TimingAndInput.cmake)
include(cmake/tests/Rendering.cmake)
include(cmake/tests/GameplayAndServices.cmake)
include(cmake/tests/RiderSkins.cmake)
include(cmake/tests/RiderSkinRender.cmake)
include(cmake/tests/RiderSkinPreview.cmake)
include(tests/rr64_diagnostic_options_fixture.cmake)
include(tests/rr64_startup_recovery_fixture.cmake)
include(tests/rr64_music_ui_fixture.cmake)
