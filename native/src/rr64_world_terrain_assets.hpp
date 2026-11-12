#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rr64::world {

struct TerrainAssetRelocation {
    std::uint32_t word_offset = 0;
    std::uint32_t target_offset = 0;
    // Only segment-seven bindings have a texture index. Animated frames are
    // contiguous at target_offset + frame * frame_stride in the upload blob.
    std::uint16_t texture_index = 0xffffu;
};

struct TerrainTextureAsset {
    std::uint32_t raw_offset = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t frame_stride = 0;
    std::uint16_t frame_count = 0;
    std::uint16_t flags = 0;
};

struct TerrainCellAsset {
    std::uint32_t cell_index = 0;
    std::uint32_t partition_id = 0;
    std::array<float, 2> authored_origin{};
    std::array<float, 4> root_quaternion{};
    // Preserve the original four header fields; they are not assumed to be a
    // useful AABB (the locked ROM includes +/-70000 placeholder values).
    std::array<float, 4> original_bounds{};
    // Local raw-vertex bounds, before the original root and cell translation.
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    std::uint32_t display_list_offset = 0;
    std::uint32_t display_list_size = 0;
    std::uint32_t vertices = 0;
    std::uint32_t triangles = 0;
};

struct TerrainAssets {
    // Big-endian N64 bytes. Upload with the usual guest-byte XOR3 conversion,
    // then patch each word to the allocation's physical base + target_offset.
    // No guest addresses, caller memory, or ROM span are retained by this value.
    std::vector<std::uint8_t> bytes;
    std::vector<TerrainAssetRelocation> relocations;
    std::vector<TerrainTextureAsset> textures;
    std::vector<TerrainCellAsset> cells;
    std::uint32_t packets = 0;
    std::uint32_t vertices = 0;
    std::uint32_t triangles = 0;
    std::uint32_t non_render_packets = 0;
};

// Bounded, transactional construction from the already-loaded big-endian ROM.
// Materials execute the original pure command writers on private RDRAM. The
// original terrain loader, 96-slot pool, simulation and collision stay untouched.
// Batching wraps each packet's unchanged triangles in the RR64 renderer command.
bool build_terrain_assets(std::span<const std::uint8_t> rom, TerrainAssets &output,
                          std::string &error) noexcept;
bool build_terrain_assets(std::span<const std::uint8_t> rom, TerrainAssets &output,
                          std::string &error, bool batch_triangles) noexcept;

bool build_terrain_assets(std::span<const std::uint8_t> rom, TerrainAssets &output,
                          std::string &error, bool batch_triangles, bool compile_packets) noexcept;

} // namespace rr64::world
