#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rr64::world {

struct ObjectAssetRelocation {
    std::uint32_t word_offset = 0;
    std::uint32_t target_offset = 0;
    // Animated texture pointers retain their first-frame offset. The driver
    // adds frame * frame_stride without changing any original texture state.
    std::uint32_t texture_index = 0xffffffffu;
};
struct ObjectTextureAsset {
    std::uint32_t raw_offset = 0, raw_size = 0, frame_stride = 0;
    std::uint16_t frame_count = 0, flags = 0;
    float frame_duration_ms = 0.0f;
    std::uint32_t model_index = 0, model_local_offset = 0;
};
struct ObjectModelAsset {
    std::uint32_t raw_rom_offset = 0;
    std::uint32_t root_source_offset = 0;
    std::uint32_t display_list_offset = 0, display_list_size = 0;
    std::uint32_t vertices = 0, triangles = 0;
    std::uint16_t source_bank = 0;
    // Bounds include all static child transforms, in this source bank's units.
    std::array<float, 3> minimum{}, maximum{};
};
struct ObjectPlacementAsset {
    std::uint32_t raw_rom_offset = 0, cell_index = 0, placement_index = 0;
    std::uint16_t descriptor = 0, model_index = 0;
    std::uint8_t category = 0;
    bool billboard = false;
    std::array<float, 4> quaternion{};
    std::array<float, 3> position{};
};
struct ObjectAssets {
    // Big-endian upload bytes, independent of ROM and all live guest memory.
    // Each model list multiplies/pushes its original static child matrices and
    // pops them in original order. The caller supplies the per-placement root.
    std::vector<std::uint8_t> bytes;
    std::vector<ObjectAssetRelocation> relocations;
    std::vector<ObjectTextureAsset> textures;
    std::vector<ObjectModelAsset> models;
    std::vector<ObjectPlacementAsset> placements;
    std::uint32_t packets = 0, vertices = 0, triangles = 0;
};

// Batching wraps each packet's unchanged triangles in the RR64 renderer command.
bool build_object_assets(std::span<const std::uint8_t> rom, ObjectAssets &output,
                         std::string &error) noexcept;
bool build_object_assets(std::span<const std::uint8_t> rom, ObjectAssets &output,
                         std::string &error, bool batch_triangles) noexcept;

bool build_object_assets(std::span<const std::uint8_t> rom, ObjectAssets &output,
                         std::string &error, bool batch_triangles, bool compile_packets) noexcept;

// Compile only these original resource-table indices, in the supplied order.
// Uses the same native material/child writers; does not parse course placements.
bool build_object_model_assets(std::span<const std::uint8_t> rom,
                               std::span<const unsigned> resources, ObjectAssets &output,
                               std::string &error) noexcept;

} // namespace rr64::world
