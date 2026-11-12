#include "rr64_music_media.hpp"
#include <cmath>
#include <iostream>

// Fixture audio is synthesized by the developer, never extracted from the ROM.
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path directory = argv[1];
    for (const char* extension : {"flac", "mp3", "m4a", "aac", "wma"}) {
        const auto path = directory / (std::string("tone.") + extension);
        const auto pcm = rr64::music::decode_media(path, 96000);
        // One second of synthesized tone, allowing codec padding at the ends.
        if (pcm.size() < 90000 || pcm.size() > 110000 || pcm.size() % 2) {
            std::cerr << extension << ": unexpected decoded size " << pcm.size() << '\n';
            return 3;
        }
        double energy = 0;
        for (auto sample : pcm) energy += double(sample) * sample;
        if (energy / pcm.size() < 1000000) return 4;
        if (!rr64::music::decode_media(path, 16).empty()) return 5;
        std::cout << extension << ": stereo 48kHz decode and size limit passed\n";
    }
    if (!rr64::music::decode_media(directory / "missing.mp3", 96000).empty()) return 6;
    if (!rr64::music::decode_media(directory / "invalid.mp3", 96000).empty()) return 7;
    return 0;
}
