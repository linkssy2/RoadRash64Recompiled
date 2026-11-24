# Build AppIcon.icns from the existing launcher icon (run with cmake -P).
#   -D SOURCE=<png>  -D OUTPUT=<icns>  -D WORK=<scratch folder>
# sips and iconutil ship with macOS; no extra tools are needed.
file(REMOVE_RECURSE "${WORK}/AppIcon.iconset")
file(MAKE_DIRECTORY "${WORK}/AppIcon.iconset")
foreach(SIZE 16 32 128 256 512)
    math(EXPR DOUBLE "${SIZE} * 2")
    execute_process(COMMAND sips -z ${SIZE} ${SIZE} "${SOURCE}" --out "${WORK}/AppIcon.iconset/icon_${SIZE}x${SIZE}.png"
        OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND sips -z ${DOUBLE} ${DOUBLE} "${SOURCE}" --out "${WORK}/AppIcon.iconset/icon_${SIZE}x${SIZE}@2x.png"
        OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
endforeach()
execute_process(COMMAND iconutil -c icns "${WORK}/AppIcon.iconset" -o "${OUTPUT}" COMMAND_ERROR_IS_FATAL ANY)
