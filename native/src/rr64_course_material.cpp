#include "rr64_course_material.hpp"

#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_experimental_course.hpp"
#include <cstring>
#include <stdexcept>

namespace {
constexpr unsigned guest_size = 32u * 1024u * 1024u;
unsigned physical(unsigned address, unsigned size) {
    const auto offset = address & 0x1fffffffu;
    if ((address & 0xe0000000u) != 0x80000000u || offset > guest_size ||
        size > guest_size - offset) {
        throw std::runtime_error("private course material address is outside RDRAM");
    }
    return offset;
}
unsigned word(const unsigned char *memory, unsigned address) {
    unsigned value;
    std::memcpy(&value, memory + physical(address, 4), 4);
    return value;
}
void word(unsigned char *memory, unsigned address, unsigned value) {
    std::memcpy(memory + physical(address, 4), &value, 4);
}
unsigned features(unsigned char *memory, unsigned texture) {
    physical(texture, 0x40);
    const unsigned extension = word(memory, texture + 0x38);
    if (word(memory, texture + 0x34) != 16 ||
        !rr64::experimental_course::valid_material_word(extension)) {
        return 0;
    }
    return extension;
}
void emit(unsigned char *memory, unsigned pointer, unsigned first, unsigned second) {
    const auto output = word(memory, pointer);
    physical(output, 8);
    word(memory, output, first);
    word(memory, output + 4, second);
    word(memory, pointer, output + 8);
}
unsigned native_cull_mask(const unsigned char *memory) {
    // Match native 8000B260: the normal race frame uses FRONT, because its
    // authored terrain/model winding is opposite the imported source's GBI.
    // Respect the original frame switches instead of assuming BACK or ORing
    // another cull bit into an existing mode (which could cull both faces).
    unsigned short front = 0, back = 0;
    std::memcpy(&front, memory + (physical(0x8009cd82u, 2) ^ 2u), 2);
    std::memcpy(&back, memory + (physical(0x8009cd84u, 2) ^ 2u), 2);
    return ((unsigned(front) << 9u) | (unsigned(back) << 10u)) & 0x600u;
}
unsigned native_fog_mask(const unsigned char *memory) {
    // Native 8000B260 gets G_FOG from this original frame switch. Do not
    // restore fog unconditionally: callers can also author a fog-free frame.
    unsigned short fog = 0;
    std::memcpy(&fog, memory + (physical(0x8009cd88u, 2) ^ 2u), 2);
    return (unsigned(fog) << 16u) & 0x10000u;
}
} // namespace
#endif

extern "C" void rr64_course_material_begin(unsigned char *memory, unsigned texture,
                                           unsigned pointer) noexcept(false) {
#ifdef RR64_EXPERIMENTAL_COURSE
    if (!rr64::experimental_course::installed())
        return;
    const auto flags = features(memory, texture);
    if (!flags)
        return;
    // RGBA16 must explicitly leave a preceding CI material's TLUT mode. The
    // original RGBA16 branch lacks that reset. Keep native loading/format math.
    emit(memory, pointer, 0xe7000000u, 0u); // PipeSync
    emit(memory, pointer, 0xe3001001u, 0u); // TextureLUT = none
    if (flags & rr64::experimental_course::material_alpha) {
        // These source materials use shade alpha for opacity. Native G_FOG
        // replaces it with fog density (or zero when user fog is disabled),
        // making near surfaces such as Rainbow Road disappear. Scope fog off
        // through vertex loading and use a non-fog first-cycle pass instead.
        emit(memory, pointer, 0xd9feffffu, 0u); // Clear G_FOG.
        emit(memory, pointer, 0xfc1219ffu, 0xfffffe38u);
        unsigned mode = 0x0c1849d8u; // G_RM_PASS | G_RM_AA_ZB_XLU_SURF2.
        // Sherbet Land's source ice writes depth; its submerged/edge pass
        // instead uses interpenetrating depth comparison. Keep those explicit
        // source distinctions without changing ordinary translucent terrain.
        if (flags & rr64::experimental_course::material_alpha_depth_write)
            mode |= 0x20u; // Z_UPD.
        if (flags & rr64::experimental_course::material_alpha_interpenetrating)
            mode = (mode & ~0xc00u) | 0x400u; // ZMODE_INTER.
        emit(memory, pointer, 0xe200001cu, mode);
    }
    // Converted triangle order matches native terrain. Source two-sided
    // materials disable culling; other materials use the native frame mask.
    emit(memory, pointer, 0xd9fff9ffu, (flags & 0x10u) ? 0u : native_cull_mask(memory));
    emit(memory, pointer, 0xd7000002u, (flags & 0x20u) ? 0xffffffffu : 0x80008000u);
#else
    (void)memory;
    (void)texture;
    (void)pointer;
#endif
}

extern "C" unsigned rr64_course_material_tile(unsigned char *memory, unsigned texture,
                                              unsigned tile) noexcept(false) {
#ifdef RR64_EXPERIMENTAL_COURSE
    if (rr64::experimental_course::installed()) {
        const auto flags = features(memory, texture);
        if (flags) {
            // gDPSetTile word1: cmt at18, cms at8. Preserve native masks,
            // shifts, tile and palette; source mirror and clamp are independent.
            tile = (tile & ~0x000c0300u) | ((flags & 3u) << 8u) | (((flags >> 2u) & 3u) << 18u);
        }
    }
#else
    (void)memory;
    (void)texture;
#endif
    return tile;
}

extern "C" void rr64_course_material_end(unsigned char *memory, unsigned pointer) noexcept(false) {
#ifdef RR64_EXPERIMENTAL_COURSE
    if (!rr64::experimental_course::installed())
        return;
    // Close every terrain submesh, including untextured ones, with the stock
    // culling and scale. No mutable CPU-side material state can escape a build.
    emit(memory, pointer, 0xe7000000u, 0u);
    emit(memory, pointer, 0xfc127fffu, 0xfffdf238u); // Native fogged opaque material.
    emit(memory, pointer, 0xe200001cu, 0xc8112078u);
    // Native material producer caches its last mode on the CPU. Our explicit
    // restoration must invalidate that cache too, including the size pass.
    word(memory, 0x800b1a20u, 0xffffffffu);
    emit(memory, pointer, 0xd9fff9ffu, native_cull_mask(memory));
    emit(memory, pointer, 0xd9feffffu, native_fog_mask(memory));
    emit(memory, pointer, 0xd7000002u, 0x80008000u);
#else
    (void)memory;
    (void)pointer;
#endif
}

extern "C" unsigned rr64_course_material_format(unsigned char *memory, unsigned texture,
                                                unsigned command) noexcept(false) {
#ifdef RR64_EXPERIMENTAL_COURSE
    if (rr64::experimental_course::installed() && (features(memory, texture) & 0x40u))
        return (command & ~0x00e00000u) | 0x00600000u; // G_IM_FMT_IA = 3.
#else
    (void)memory;
    (void)texture;
#endif
    return command;
}

#ifdef RR64_EXPERIMENTAL_COURSE
#include "librecomp/addresses.hpp"
#include <algorithm>
namespace {
constexpr unsigned scratch_capacity = 64u * 1024u;
constexpr unsigned scratch_guard = 16;
unsigned char* scratch_memory = nullptr;
unsigned scratch_base = 0;
unsigned scratch_bound = 0;
unsigned half(const unsigned char* memory, unsigned address) {
    std::uint16_t value;
    std::memcpy(&value, memory + (physical(address, 2) ^ 2u), 2);
    return value;
}
void scratch_guards(unsigned char* memory) {
    const auto before = physical(scratch_base - scratch_guard, scratch_guard);
    const auto after = physical(scratch_base + scratch_capacity, scratch_guard);
    for (unsigned i = 0; i < scratch_guard; ++i)
        if (memory[before+i] != 0xa5 || memory[after+i] != 0xa5)
            throw std::runtime_error("course sizing scratch guard changed");
}
unsigned sizing_bound(unsigned char* memory, unsigned cell) {
    physical(cell, 0xf0);
    const unsigned bytes = word(memory, cell+4);
    physical(cell, bytes);
    const unsigned count = half(memory, cell+12);
    auto region = [cell, bytes](std::uint64_t offset, std::uint64_t size) {
        if (offset > bytes || size > bytes-offset)
            throw std::runtime_error("course sizing cell references outside its record");
        return cell + static_cast<unsigned>(offset);
    };
    if (word(memory, cell) != 0x3f || bytes < 0xf0)
        throw std::runtime_error("course sizing requires a native terrain cell");
    region(0xf0, std::uint64_t(count)*12);
    std::uint64_t bound = 16; // Initial matrix command and final EndDL.
    for (unsigned i = 0; i < count; ++i) {
        const unsigned ref_offset = 0xf0+i*12;
        const unsigned ref = cell+ref_offset;
        const std::uint64_t offset = std::uint64_t(ref_offset)+word(memory, ref);
        const unsigned sub = region(offset, 0x18);
        const unsigned sub_bytes = word(memory, sub+4);
        region(offset, sub_bytes);
        if (sub_bytes < 0x18 || word(memory, sub) != 0x3d ||
            word(memory, ref+4) != sub_bytes)
            throw std::runtime_error("course sizing invalid native submesh");
        const unsigned packets = half(memory, sub+14);
        if (packets > half(memory, sub+12))
            throw std::runtime_error("course sizing render packet count exceeds total");
        if (!packets) continue;
        // 10FD0 and bounded material branches have no geometry-dependent
        // command loop. 128 commands exceeds every path, including CI palettes
        // and imported alpha state/restoration. Packet output is counted below.
        bound += 1024;
        std::uint64_t cursor = 0x18;
        for (unsigned j = 0; j < packets; ++j) {
            if (cursor > sub_bytes || 8 > sub_bytes-cursor)
                throw std::runtime_error("course sizing packet header outside submesh");
            const unsigned p = region(offset+cursor, 8);
            const unsigned triangles = half(memory, p);
            const unsigned vertices = half(memory, p+2);
            const unsigned packet_bytes = half(memory, p+4);
            const unsigned vertex_bytes = half(memory, p+6);
            if (!vertices || vertices > 32 || vertex_bytes != vertices*16 ||
                packet_bytes < 8u+vertex_bytes+triangles*2u ||
                packet_bytes > sub_bytes-cursor || (packet_bytes & 7u))
                throw std::runtime_error("course sizing invalid native vertex packet");
            // One vertex command plus at most one command per triangle.
            bound += 64u + std::uint64_t(triangles)*8u;
            if (bound > scratch_capacity)
                throw std::runtime_error("course sizing command bound exceeds 64KiB");
            cursor += packet_bytes;
        }
    }
    if (bound > scratch_capacity)
        throw std::runtime_error("course sizing command bound exceeds 64KiB");
    return static_cast<unsigned>(bound);
}
} // namespace
#endif

extern "C" unsigned rr64_course_material_scratch(unsigned char* memory, unsigned cell) noexcept(false) {
#ifdef RR64_EXPERIMENTAL_COURSE
    if (!rr64::experimental_course::installed()) return 0;
    const unsigned bound = sizing_bound(memory, cell); // Before allocation/writes.
    if (scratch_base) {
        if (scratch_memory != memory)
            throw std::runtime_error("course sizing scratch session was not reset");
        scratch_guards(memory);
    } else {
        auto* allocation = static_cast<unsigned char*>(recomp::alloc(memory, scratch_capacity+2*scratch_guard));
        if (!allocation) throw std::runtime_error("cannot allocate course sizing scratch");
        const auto offset = allocation-memory;
        if (offset < 0 || static_cast<std::uint64_t>(offset)+scratch_capacity+2*scratch_guard > guest_size || (offset & 7)) {
            recomp::free(memory, allocation);
            throw std::runtime_error("course sizing allocation outside supported guest memory");
        }
        scratch_memory = memory;
        scratch_base = 0x80000000u+static_cast<unsigned>(offset)+scratch_guard;
        std::fill_n(allocation, scratch_guard, 0xa5);
        std::fill_n(allocation+scratch_guard+scratch_capacity, scratch_guard, 0xa5);
    }
    scratch_bound = bound;
    return scratch_base;
#else
    (void)memory; (void)cell; return 0;
#endif
}

extern "C" void rr64_course_material_scratch_end(unsigned char* memory, unsigned end, unsigned bytes) noexcept(false) {
#ifdef RR64_EXPERIMENTAL_COURSE
    if (!rr64::experimental_course::installed()) return;
    if (!scratch_base || scratch_memory != memory || bytes > scratch_bound ||
        end != scratch_base+bytes)
        throw std::runtime_error("course sizing exceeded its verified bound");
    scratch_guards(memory);
#else
    (void)memory; (void)end; (void)bytes;
#endif
}

extern "C" void rr64_course_material_reset_scratch(void) {
#ifdef RR64_EXPERIMENTAL_COURSE
    scratch_memory = nullptr; scratch_base = 0; scratch_bound = 0;
#endif
}
