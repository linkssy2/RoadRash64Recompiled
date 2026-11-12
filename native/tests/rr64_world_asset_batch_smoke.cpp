#include "rr64_world_packet_compiler.hpp"
#include "rr64_world_course_regions.hpp"
#include "rr64_world_terrain_assets.hpp"
#include "rr64_world_object_assets.hpp"
#include "recomp.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" void func_8000CD34(std::uint8_t*, recomp_context*) { std::abort(); }
namespace {
using namespace rr64::world;
std::uint32_t word(const std::vector<std::uint8_t>& b, std::size_t p) {
    return (std::uint32_t(b[p]) << 24) | (std::uint32_t(b[p + 1]) << 16) |
        (std::uint32_t(b[p + 2]) << 8) | b[p + 3];
}
struct Normalizer {
    std::vector<std::uint32_t> headers;
    std::uint64_t triangles = 0;
    bool scan(const std::vector<std::uint8_t>& bytes, std::uint32_t offset, std::uint32_t size) {
        if (offset > bytes.size() || size > bytes.size() - offset || (size & 7u)) { return false; }
        const auto end = offset + size;
        for (auto p = offset; p < end; p += 8u) {
            const auto a = word(bytes, p), op = a >> 24u;
            if (a == 0x64000034u) {
                const auto count = word(bytes, p + 4u);
                if (!count || count > 32768u || count > (end - p - 8u) / 8u ||
                    p == offset || (word(bytes, p - 8u) >> 24u) != 0x01u) { return false; }
                headers.push_back(p);
                for (std::uint32_t i = 0; i < count; ++i) {
                    p += 8u; const auto tri_op = word(bytes, p) >> 24u;
                    if (tri_op != 0x05u && tri_op != 0x06u) { return false; }
                    triangles += tri_op == 0x06u ? 2u : 1u;
                }
            }
            else if (op == 0x05u || op == 0x06u) { return false; }
        }
        return true;
    }
    std::uint32_t map(std::uint32_t p) const {
        return p - 8u * std::uint32_t(std::lower_bound(headers.begin(), headers.end(), p) - headers.begin());
    }
    bool bytesEqual(const std::vector<std::uint8_t>& original, const std::vector<std::uint8_t>& batch) const {
        if (batch.size() != original.size() + headers.size() * 8u) { return false; }
        std::size_t h = 0, o = 0;
        for (std::size_t p = 0; p < batch.size();) {
            if (h < headers.size() && p == headers[h]) { ++h; p += 8u; }
            else if (o == original.size() || original[o++] != batch[p++]) { return false; }
        }
        return o == original.size();
    }
};
template<class Assets> bool common(const Assets& a, const Assets& b, const Normalizer& n) {
    if (!n.bytesEqual(a.bytes, b.bytes) || a.packets != b.packets || a.vertices != b.vertices ||
        a.triangles != b.triangles || a.triangles != n.triangles || a.relocations.size() != b.relocations.size() ||
        a.textures.size() != b.textures.size()) { return false; }
    for (std::size_t i = 0; i < a.relocations.size(); ++i) {
        const auto& x = a.relocations[i]; const auto& y = b.relocations[i];
        if (x.word_offset != n.map(y.word_offset) || x.target_offset != n.map(y.target_offset) ||
            x.texture_index != y.texture_index) { return false; }
    }
    for (std::size_t i = 0; i < a.textures.size(); ++i) {
        const auto& x = a.textures[i]; const auto& y = b.textures[i];
        if (x.raw_offset != n.map(y.raw_offset) || x.raw_size != y.raw_size || x.frame_stride != y.frame_stride ||
            x.frame_count != y.frame_count || x.flags != y.flags) { return false; }
    }
    return true;
}
bool compare(const TerrainAssets& a, const TerrainAssets& b, Normalizer& n) {
    if (a.cells.size() != b.cells.size() || a.non_render_packets != b.non_render_packets) { return false; }
    for (const auto& c : b.cells) { if (!n.scan(b.bytes, c.display_list_offset, c.display_list_size)) { return false; } }
    if (!common(a, b, n)) { return false; }
    for (std::size_t i = 0; i < a.cells.size(); ++i) {
        const auto& x = a.cells[i]; const auto& y = b.cells[i];
        if (x.cell_index != y.cell_index || x.partition_id != y.partition_id || x.authored_origin != y.authored_origin ||
            x.root_quaternion != y.root_quaternion || x.original_bounds != y.original_bounds || x.minimum != y.minimum ||
            x.maximum != y.maximum || x.vertices != y.vertices || x.triangles != y.triangles ||
            x.display_list_offset != n.map(y.display_list_offset) ||
            x.display_list_size != n.map(y.display_list_offset + y.display_list_size) - n.map(y.display_list_offset)) { return false; }
    }
    return true;
}
bool compare(const ObjectAssets& a, const ObjectAssets& b, Normalizer& n) {
    if (a.models.size() != b.models.size() || a.placements.size() != b.placements.size()) { return false; }
    for (const auto& m : b.models) { if (!n.scan(b.bytes, m.display_list_offset, m.display_list_size)) { return false; } }
    if (!common(a, b, n)) { return false; }
    for (std::size_t i = 0; i < a.models.size(); ++i) {
        const auto& x = a.models[i]; const auto& y = b.models[i];
        if (x.raw_rom_offset != y.raw_rom_offset || x.root_source_offset != y.root_source_offset ||
            x.vertices != y.vertices || x.triangles != y.triangles || x.source_bank != y.source_bank ||
            x.minimum != y.minimum || x.maximum != y.maximum || x.display_list_offset != n.map(y.display_list_offset) ||
            x.display_list_size != n.map(y.display_list_offset + y.display_list_size) - n.map(y.display_list_offset)) { return false; }
    }
    for (std::size_t i = 0; i < a.textures.size(); ++i) {
        const auto& x = a.textures[i]; const auto& y = b.textures[i];
        if (x.frame_duration_ms != y.frame_duration_ms || x.model_index != y.model_index || x.model_local_offset != y.model_local_offset) { return false; }
    }
    for (std::size_t i = 0; i < a.placements.size(); ++i) {
        const auto& x = a.placements[i]; const auto& y = b.placements[i];
        if (x.raw_rom_offset != y.raw_rom_offset || x.cell_index != y.cell_index || x.placement_index != y.placement_index ||
            x.descriptor != y.descriptor || x.model_index != y.model_index || x.category != y.category ||
            x.billboard != y.billboard || x.quaternion != y.quaternion || x.position != y.position) { return false; }
    }
    return true;
}
}
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) { return 2; }
    if (argc == 3 && std::string(argv[2]) == "--geometry-audit") {
        std::ifstream stream(argv[1],std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)),{});
        TerrainAssets terrain;ObjectAssets objects;std::string error;
        if(!build_terrain_assets(bytes,terrain,error,true)||!build_object_assets(bytes,objects,error,true)){
            std::fprintf(stderr,"audit failed: %s\n",error.c_str());return 1;
        }
        PacketCompiler tc,oc;tc.compile(terrain,terrain.cells);oc.compile(objects,objects.models);
        const auto report=[](const char* name,const PacketCompiler::Stats& stats){
            std::printf("%s packets_before=%llu packets_after=%llu vertices_before=%llu vertices_after=%llu triangles=%llu\n",name,
                (unsigned long long)stats.packetsBefore,(unsigned long long)stats.packetsAfter,
                (unsigned long long)stats.verticesBefore,(unsigned long long)stats.verticesAfter,(unsigned long long)stats.triangles);
        };report("terrain",tc.stats);report("scenery_unique_models",oc.stats);
        CourseRegions regions;CourseRegions::Mask occupied{};
        for(const auto& cell:terrain.cells)occupied[cell.cell_index]=true;
        regions.build(occupied);std::array<unsigned,CourseRegions::count+1> sizes{};
        for(unsigned value:regions.region)if(value)++sizes[value];
        std::sort(sizes.begin(),sizes.end(),std::greater<unsigned>());
        for(unsigned i=0;i<sizes.size()&&sizes[i];++i)std::printf("connected_island_rank=%u cells=%u\n",i+1,sizes[i]);
        unsigned billboards=0;for(const auto& placement:objects.placements)billboards+=placement.billboard;
        std::printf("scenery models=%zu placements=%zu billboards=%u terrain_cells=%zu\n",objects.models.size(),objects.placements.size(),billboards,terrain.cells.size());
        return 0;
    }
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(input)), {});
    TerrainAssets terrain, terrainBatch; ObjectAssets objects, objectsBatch; std::string error;
    if (!build_terrain_assets(rom, terrain, error) || !build_terrain_assets(rom, terrainBatch, error, true) ||
        !build_object_assets(rom, objects, error) || !build_object_assets(rom, objectsBatch, error, true)) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str()); return 1;
    }
    Normalizer tn, on;
    if (!compare(terrain, terrainBatch, tn) || !compare(objects, objectsBatch, on)) {
        std::fputs("FAIL: batched asset bytes, metadata, or command bounds differ\n", stderr); return 1;
    }
    // Corrupt the first header length and ensure a packet cannot consume state.
    if (tn.headers.empty()) { return 1; }
    terrainBatch.bytes[tn.headers[0] + 4u] = 0xffu;
    Normalizer invalid;
    if (compare(terrain, terrainBatch, invalid)) { std::fputs("FAIL: invalid header accepted\n", stderr); return 1; }
    std::printf("World asset batching: PASS terrain_headers=%zu object_headers=%zu terrain_triangles=%llu object_triangles=%llu exact_bytes_and_metadata=1 malformed_header_rejected=1\n",
        tn.headers.size(), on.headers.size(), tn.triangles, on.triangles);
    return 0;
}
