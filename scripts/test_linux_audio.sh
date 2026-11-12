#!/usr/bin/env bash
set -euo pipefail
# Developer-only fixture generation. The player package does not need ffmpeg CLI.
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
work="${1:-$repo/native/build/audio-fixtures}"
mkdir -p "$work"
for spec in 'flac:flac' 'mp3:libmp3lame' 'm4a:aac' 'aac:aac' 'wma:wmav2'; do
    extension=${spec%%:*}
    codec=${spec#*:}
    ffmpeg -hide_banner -loglevel error -y -f lavfi \
        -i 'sine=frequency=440:duration=1:sample_rate=48000' -ac 2 \
        -c:a "$codec" "$work/tone.$extension"
done
printf 'invalid developer fixture\n' > "$work/invalid.mp3"
"${RR64_NATIVE_BUILD_DIR:-$repo/native/build}/bin/RR64MusicSmoke" "$work"
