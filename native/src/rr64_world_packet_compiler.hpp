#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <algorithm>

namespace rr64::world {
// One-time compilation of renderer-owned, validated native display lists.
// Only adjacent VTX + triangle packets are combined. Every state command is
// a barrier; no triangle may move across a material, matrix or root boundary.
struct PacketCompiler {
    using Vertex = std::array<uint8_t, 16>;
    struct Stats {
        uint64_t packetsBefore = 0, packetsAfter = 0, verticesBefore = 0, verticesAfter = 0,
                 triangles = 0;
    } stats;
    static uint32_t word(const std::vector<uint8_t> &b, uint32_t p) {
        if (uint64_t(p) + 4 > b.size())
            throw std::runtime_error("packet compiler word range");
        return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 | uint32_t(b[p + 2]) << 8 | b[p + 3];
    }
    static void append(std::vector<uint8_t> &b, uint32_t v) {
        for (int s = 24; s >= 0; s -= 8)
            b.push_back(uint8_t(v >> s));
    }
    template <class Assets, class Ranges> void compile(Assets &assets, Ranges &lists) {
        using Relocation = typename decltype(assets.relocations)::value_type;
        std::unordered_map<uint32_t, Relocation> relocations;
        for (const auto &r : assets.relocations)
            if (!relocations.emplace(r.word_offset, r).second)
                throw std::runtime_error("duplicate relocation");
        std::vector<Relocation> result;
        for (auto &list : lists) {
            const uint64_t limit = uint64_t(list.display_list_offset) + list.display_list_size;
            if (limit > assets.bytes.size() || (list.display_list_size & 7))
                throw std::runtime_error("packet list range");
            uint32_t p = list.display_list_offset, end = uint32_t(limit);
            // Preserve entire unsupported lists, including empty VTX packets.
            bool supported = true;
            for (uint32_t q = p; q < end; q += 8) {
                const uint32_t a = word(assets.bytes, q);
                if ((a >> 24) == 1) {
                    const uint32_t n = (a >> 12) & 255;
                    if (!n || n > 32 || ((a >> 1) & 127) != n || q + 8 >= end ||
                        word(assets.bytes, q + 8) != 0x64000034u) {
                        supported = false;
                        break;
                    }
                }
                if (a == 0x64000034u) {
                    const uint32_t n = word(assets.bytes, q + 4);
                    if (!n || n > 32768 || uint64_t(q) + 8 + uint64_t(n) * 8 > end)
                        throw std::runtime_error("packet batch range");
                    q += n * 8;
                }
            }
            if (!supported) {
                for (const auto &r : assets.relocations)
                    if (r.word_offset >= p && r.word_offset < end)
                        result.push_back(r);
                continue;
            }
            std::vector<uint8_t> commands;
            std::vector<Relocation> local;
            std::vector<Vertex> vertices;
            std::vector<std::array<uint32_t, 3>> triangles;
            auto command = [&](uint32_t a, uint32_t b) {
                append(commands, a);
                append(commands, b);
            };
            auto flush = [&]() {
                if (vertices.empty())
                    return;
                while (assets.bytes.size() % 8)
                    assets.bytes.push_back(0);
                const uint32_t address = uint32_t(assets.bytes.size());
                for (const auto &v : vertices)
                    assets.bytes.insert(assets.bytes.end(), v.begin(), v.end());
                Relocation r{};
                r.word_offset = uint32_t(commands.size() + 4);
                r.target_offset = address;
                local.push_back(r);
                command(0x01000000u | (uint32_t(vertices.size()) << 12) |
                            (uint32_t(vertices.size()) * 2),
                        0);
                if (!triangles.empty()) {
                    command(0x64000034u, uint32_t((triangles.size() + 1) / 2));
                    auto packed = [](const auto &t) { return t[0] << 17 | t[1] << 9 | t[2] << 1; };
                    size_t i = 0;
                    if (triangles.size() % 2)
                        command(0x05000000u | packed(triangles[i++]), 0);
                    for (; i < triangles.size(); i += 2)
                        command(0x06000000u | packed(triangles[i]), packed(triangles[i + 1]));
                }
                stats.packetsAfter++;
                stats.verticesAfter += vertices.size();
                vertices.clear();
                triangles.clear();
            };
            while (p < end) {
                uint32_t a = word(assets.bytes, p), b = word(assets.bytes, p + 4);
                if ((a >> 24) == 1) {
                    const uint32_t n = (a >> 12) & 255, finish = (a >> 1) & 127;
                    auto rel = relocations.find(p + 4);
                    if (n == 0 || n > 32 || finish != n || rel == relocations.end() ||
                        uint64_t(rel->second.target_offset) + n * 16 > assets.bytes.size())
                        throw std::runtime_error("uncertified vertex packet");
                    uint32_t next = p + 8;
                    if (next >= end || word(assets.bytes, next) != 0x64000034u)
                        throw std::runtime_error("missing triangle packet");
                    uint32_t count = word(assets.bytes, next + 4);
                    if (!count || count > 32768 || uint64_t(next) + 8 + uint64_t(count) * 8 > end)
                        throw std::runtime_error("triangle packet range");
                    // Flush using the upper bound before remapping, so no
                    // partially constructed remap can refer to a prior group.
                    if (vertices.size() + n > 127 || triangles.size() + uint64_t(count) * 2 > 65536)
                        flush();
                    std::array<uint32_t, 32> remap{};
                    for (uint32_t i = 0; i < n; i++) {
                        Vertex v;
                        std::copy_n(assets.bytes.begin() + rel->second.target_offset + i * 16, 16,
                                    v.begin());
                        auto found = std::find(vertices.begin(), vertices.end(), v);
                        remap[i] = uint32_t(found - vertices.begin());
                        if (found == vertices.end())
                            vertices.push_back(v);
                    }
                    auto tri = [&](uint32_t w) {
                        std::array<uint32_t, 3> t{};
                        for (unsigned k = 0; k < 3; k++) {
                            auto index = (w >> (17 - k * 8)) & 127;
                            if (index >= n)
                                throw std::runtime_error("triangle index");
                            t[k] = remap[index];
                        }
                        triangles.push_back(t);
                        stats.triangles++;
                    };
                    for (uint32_t c = 0; c < count; c++) {
                        uint32_t at = next + 8 + c * 8, w = word(assets.bytes, at), op = w >> 24;
                        if (op != 5 && op != 6)
                            throw std::runtime_error("packet state command");
                        tri(w);
                        if (op == 6)
                            tri(word(assets.bytes, at + 4));
                    }
                    stats.packetsBefore++;
                    stats.verticesBefore += n;
                    p = next + 8 + count * 8;
                    continue;
                }
                flush();
                if (auto rel = relocations.find(p + 4); rel != relocations.end()) {
                    auto r = rel->second;
                    r.word_offset = uint32_t(commands.size() + 4);
                    local.push_back(r);
                }
                command(a, b);
                p += 8;
            }
            flush();
            while (assets.bytes.size() % 8)
                assets.bytes.push_back(0);
            list.display_list_offset = uint32_t(assets.bytes.size());
            list.display_list_size = uint32_t(commands.size());
            assets.bytes.insert(assets.bytes.end(), commands.begin(), commands.end());
            for (auto r : local) {
                r.word_offset += list.display_list_offset;
                result.push_back(r);
            }
            if (assets.bytes.size() > 64u * 1024u * 1024u)
                throw std::runtime_error("packet compiler budget");
        }
        assets.relocations = std::move(result);
    }
};
}
