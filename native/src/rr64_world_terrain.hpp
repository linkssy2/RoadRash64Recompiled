#pragma once
#include "rr64_world_terrain_assets.hpp"
#include <array>

namespace rr64::world {
using Matrix = std::array<float, 16>;
// Cell bit i is row*70+column; these are observations, not course ownership.
struct TerrainViewEvidence {
    bool valid = false;
    unsigned epoch = 0, stock = 0, extended = 0, triangles = 0, excluded = 0;
    float x = 0, y = 0;
    std::array<unsigned long long, 77> stock_union{}, extended_last{};
    // Last-pass replay data, copied under the existing terrain-cache mutex.
    // Matrices are row-major guest values; stock_last is not the historical union.
    Matrix view{}, projection{};
    float view_width = 1.0f;
    std::array<unsigned long long, 77> stock_last{};
};
struct TerrainEvidence {
    bool enabled = false;
    unsigned generation = 0;
    std::array<unsigned, 19> raw_setup{}; // mode, pending, then engine setup words
    std::array<TerrainViewEvidence, 4> views{};
};
// Identity-quaternion 7D814 convention: Z basis is halved, translation is raw.
Matrix terrain_matrix(const TerrainCellAsset &cell, float origin_x, float origin_y) noexcept;
bool terrain_in_frustum(const TerrainCellAsset &cell, const Matrix &model, const Matrix &view,
                        const Matrix &projection) noexcept;
struct TerrainStatistics {
    unsigned cached_cells = 0, cached_triangles = 0, cached_bytes = 0;
    unsigned visible_cells = 0, stock_cells = 0, drawn_triangles = 0;
    unsigned replaced_stock_cells = 0;
    unsigned course_excluded_cells = 0;
    unsigned stock_course_excluded = 0;
    unsigned long long frames = 0, refusals = 0;
    TerrainEvidence evidence{};
};
TerrainStatistics terrain_statistics() noexcept;
// Called by the existing runtime immediately before it reinitializes its heap.
void terrain_reset_session() noexcept;
}
extern "C" {
// True only after the compiled terrain cache owns distant presentation.
// Native collision/resource streaming may then retain its original near tier.
int rr64_terrain_streaming_bounded(unsigned char *rdram);
unsigned rr64_terrain_streaming_range(unsigned char *rdram, unsigned original_bits);
void rr64_world_terrain_begin(unsigned char *rdram);
unsigned rr64_world_terrain_stock_state(unsigned char *rdram, unsigned record, unsigned state);
void rr64_world_terrain_observe(unsigned char *rdram, unsigned record);
void rr64_world_terrain_draw(unsigned char *rdram);
void rr64_world_terrain_finalize(unsigned char *rdram, unsigned submitted_words);
}
