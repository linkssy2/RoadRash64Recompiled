#include "rr64_online_terrain.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_prediction_rules.hpp"
#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_experimental_course.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <stdexcept>

namespace recomp { std::span<const std::uint8_t> get_rom(); }
extern "C" void func_8001BDF8(unsigned char*, recomp_context*);

namespace {
using namespace rr64::engine;
constexpr unsigned cells = 4900, magic = 0x52525444;
constexpr unsigned descriptor_offset = 32, payload_offset = 48;
constexpr unsigned object_grid_global = 0x800dde98, object_table = 0x1265c40;
constexpr unsigned collision_header_bytes = 32, no_cell = 0xffffffffu;
struct Entry { unsigned source = 0, bytes = 0; };
struct Bank {
    unsigned char* mapping = nullptr;
    std::span<const std::uint8_t> rom;
    std::array<Entry, cells> entries{};
    std::array<Entry, cells> objects{};
    std::array<bool, cells> stock_cells{};
    unsigned grid = 0, allocation = 0, capacity = 0, header = 0;
    unsigned object_grid = 0, object_capacity = 0;
    unsigned collision_header() const { return allocation + payload_offset + capacity; }
    unsigned collision_terrain() const { return collision_header() + collision_header_bytes; }
    unsigned collision_objects() const { return collision_terrain() + capacity; }
    unsigned allocation_bytes() const {
        return payload_offset + capacity * 2 + collision_header_bytes + object_capacity;
    }
};
// Live and replay descriptors retain their own immutable ROM view. Payload and
// cache words live in low guest RAM and belong to each private replay image.
std::atomic<std::shared_ptr<const Bank>> bank;
thread_local std::shared_ptr<const Bank> replay_bank;
thread_local std::uint64_t replay_bank_epoch = 0;

std::shared_ptr<const Bank> current_bank() {
    if (!rr64::prediction::active()) return bank.load(std::memory_order_acquire);
    return replay_bank_epoch == rr64::prediction::replay_epoch ? replay_bank : nullptr;
}

unsigned word(unsigned char* m, unsigned a) {
    unsigned v = 0;
    read_u32(m, a, v);
    return v;
}
unsigned be32(std::span<const std::uint8_t> b, unsigned p) {
    return (unsigned(b[p]) << 24) | (unsigned(b[p+1]) << 16) |
           (unsigned(b[p+2]) << 8) | b[p+3];
}
unsigned be16(std::span<const std::uint8_t> b, unsigned p) {
    return (unsigned(b[p]) << 8) | b[p+1];
}
bool range(std::span<const std::uint8_t> b, unsigned p, unsigned n) {
    return p <= b.size() && n <= b.size() - p;
}
unsigned source_header() {
#ifdef RR64_EXPERIMENTAL_COURSE
    if (rr64::experimental_course::installed())
        return rr64::experimental_course::terrain_rom_offset();
#endif
    return 0x18d380;
}
bool live_source(const Bank& b) {
    const auto rom = recomp::get_rom();
    return rom.data() == b.rom.data() && rom.size() == b.rom.size() &&
           source_header() == b.header;
}
bool enabled(unsigned char* m) {
    const auto rules = rr64::prediction::physics_rules();
    const unsigned mode = word(m, globals::main_mode);
    if (!is_live_race_mode(mode) && !is_race_results_mode(mode)) return false;
    // Offline actors can leave the camera's native streaming window on stock
    // and imported courses, including detached riders after a crash.
    // Physical support must use the same immutable cell source as online
    // racers rather than treating an unloaded graphics cell as empty terrain.
    // Results still run native physics after highlights have moved streaming
    // elsewhere, so retain physical floors and buildings for that scene too.
    if (!rules.active) return true;
    return rules.active && rules.connected && rules.authoritative &&
           (rr64::prediction::active() || rules.phase == rr64::netplay::Phase::Race);
}
bool valid(const Bank& b, unsigned char* m) {
    return m && (m == b.mapping || rr64::prediction::active()) &&
           valid_guest_range(b.allocation, b.allocation_bytes()) &&
           word(m, b.allocation) == magic && word(m, b.allocation+4) == b.capacity &&
           word(m, b.allocation+8) == b.grid && word(m, globals::terrain_cell_grid) == b.grid &&
           word(m, b.allocation+24) == b.allocation_bytes() &&
           word(m, b.allocation+28) == b.object_grid &&
           (!b.object_grid || word(m, object_grid_global) == b.object_grid);
}
bool inspect_objects(Bank& b) {
    const auto rom = b.rom;
    if (!valid_guest_range(b.object_grid, cells * 16) ||
        !range(rom, 0x18d380, 24) || be32(rom, 0x18d380) != 0x3e ||
        !range(rom, object_table, 16 + cells * 4) ||
        be32(rom, object_table) != 0x48 || be32(rom, object_table+12) != 70) return false;
    const unsigned original_table=0x18d380+24+be16(rom,0x18d380+20)*12;
    if (!range(rom,original_table,cells*12)) return false;
    for (unsigned cell=0; cell<cells; ++cell) {
        // Installing the track pack relocates the WHOLE terrain bank, including
        // Big Game's unchanged cells. Classify by authored cell identity, not
        // installed/current-course globals (which a historical replay cannot use).
        const unsigned original=original_table+cell*12, relative=be32(rom,original);
        const auto terrain=b.entries[cell];
        if (relative && relative<=rom.size()-original && terrain.bytes &&
            be32(rom,original+4)==terrain.bytes && range(rom,original+relative,terrain.bytes))
            b.stock_cells[cell]=(original+relative==terrain.source) ||
                std::equal(rom.begin()+original+relative,
                    rom.begin()+original+relative+terrain.bytes,rom.begin()+terrain.source);
        const unsigned table_entry=object_table+16+cell*4, offset=be32(rom,table_entry);
        if (!offset) continue;
        if (offset>rom.size()-table_entry || !range(rom,table_entry+offset,0x88)) return false;
        const unsigned source=table_entry+offset, bytes=be32(rom,source+4), count=be32(rom,source);
        if (bytes<0x88 || bytes>8192 || (bytes&7) || count>32 ||
            !range(rom,source,bytes) || 0x88+count*0x30>bytes) return false;
        for (unsigned i=0; i<count; ++i)
            if (be16(rom,source+0x88+i*0x30+0x28)>=295) return false;
        // Native62594 follows these relative lists without further bounds
        // checks. Authenticate each reference before exposing a scratch cell.
        for (unsigned sub=0; sub<64; ++sub) {
            const unsigned entry=8+sub*2, relative=be16(rom,source+entry);
            if (!relative) continue;
            const unsigned list=entry+relative;
            if (list>bytes || bytes-list<8) return false;
            const unsigned n=be16(rom,source+list);
            if (n>count || n*2>bytes-list-8) return false;
            for (unsigned j=0; j<n; ++j) {
                const unsigned ref=list+8+j*2, back=be16(rom,source+ref);
                if (back>ref || ref-back<0x88 || (ref-back-0x88)%0x30 ||
                    (ref-back-0x88)/0x30>=count) return false;
            }
        }
        b.objects[cell]={source,bytes};
        b.object_capacity=std::max(b.object_capacity,bytes);
    }
    return true;
}
bool terrain_source_matches(const Bank& b, unsigned char* m, unsigned cell) {
    const auto e=b.entries[cell];
    std::uint16_t blocks=0; read_u16(m,b.grid+cell*16+14,blocks);
    return unsigned(blocks)*8==e.bytes &&
           word(m,b.grid+cell*16+4)==(e.bytes ? 0xb0000000u|e.source : 0);
}
bool object_source_matches(const Bank& b, unsigned char* m, unsigned cell) {
    if (!b.object_grid) return true;
    const auto e=b.objects[cell];
    return word(m,b.object_grid+cell*16+12)==e.bytes &&
           word(m,b.object_grid+cell*16+4)==(e.bytes ? 0xb0000000u|e.source : 0);
}
void copy_cell(unsigned char* m, unsigned destination, const Bank& b, Entry e) {
    for (unsigned i=0; i<e.bytes; ++i)
        m[((destination-kRdramBegin)+i)^3u]=b.rom[e.source+i];
}
bool inspect(Bank& b) {
    const unsigned header=b.header;
    const auto rom = b.rom;
    if (rom.size() > 64u*1024u*1024u || !range(rom, header, 24) ||
        be32(rom, header) != 0x3e || be16(rom, header+16) != 1000 ||
        be16(rom, header+18) != cells || rom[header+23] != 70) return false;
    const unsigned textures = be16(rom, header+20), table = header + 24;
    if (!range(rom, table, (textures+cells)*12)) return false;
    for (unsigned i=0; i<cells; ++i) {
        const unsigned entry = table + (textures+i)*12;
        const unsigned offset = be32(rom, entry), size = be32(rom, entry+4);
        if (!offset) { if (size) return false; continue; }
        if (offset > rom.size()-entry || size < 0xf0 || size > 512u*1024u ||
            (size&7) || !range(rom, entry+offset, size)) return false;
        const unsigned source = entry+offset;
        if (be32(rom, source) != 0x3f || be32(rom, source+4) != size ||
            be32(rom, source+16) > size-16) return false;
        b.entries[i] = {source, size};
        b.capacity = std::max(b.capacity, size);
    }
    return b.capacity != 0;
}
// Original1BDF8 has no safe OOM result. Check the complete pool0 chain before
// calling it, with the same eight-byte request rounding as that allocator.
bool can_allocate(unsigned char* m, unsigned bytes) {
    unsigned p = word(m, 0x800bbd00), selected = 0;
    bytes = (bytes+7u)&~7u;
    for (unsigned count=0; count<131072; ++count) {
        if (!valid_guest_range(p, 8) || (p&7)) return false;
        const unsigned size=word(m,p);
        std::uint8_t busy=0,pad=0;
        std::uint16_t alignment=0;
        read_u8(m,p+6,busy);read_u8(m,p+7,pad);read_u16(m,p+4,alignment);
        if (!size) return busy != 0 && selected != 0;
        const unsigned payload=p+8u+pad;
        if (!alignment || alignment>256 || (alignment&(alignment-1)) ||
            pad>=alignment || (payload&(alignment-1)) || (size&7) ||
            !valid_guest_range(p,8u+pad+size)) return false;
        if (!busy && size>=bytes && !selected) selected=p;
        const unsigned next=p+8u+pad+size;
        if (next<=p) return false;
        p=next;
    }
    return false;
}
void clear_caches(unsigned char* m, unsigned query) {
    // 14DE4 can try BOTH the current and previous wheel-query triangle. A
    // reusable source buffer must never leave either chain pointing at a cell
    // copied for another racer. Scalar results (height/normal/surface) survive.
    for (unsigned p=0x1c;p<=0x48;p+=4) write_u32(m,query+p,0);
    write_u16(m,query+0x18,0);write_u16(m,query+0x1a,0);
}
}

namespace rr64::online_terrain {
void reset() noexcept { bank.store({}, std::memory_order_release); }
bool immutable_cell(unsigned char* m, unsigned index,
                    std::span<const std::uint8_t>& bytes,
                    std::shared_ptr<const void>* identity) noexcept {
    bytes = {};
    if (identity) identity->reset();
    if (!m || index >= cells || rr64::prediction::active()) return false;
    const auto b = bank.load(std::memory_order_acquire);
    if (!b || !valid(*b, m)) return false;
    const unsigned mode = word(m, globals::main_mode);
    if (!is_live_race_mode(mode) && !is_race_results_mode(mode)) return false;
#ifdef RR64_EXPERIMENTAL_COURSE
    if (!rr64::experimental_course::cell_allowed(index)) return false;
#endif
    if (!live_source(*b)) return false;
    const auto entry = b->entries[index];
    std::uint16_t blocks = 0;
    read_u16(m, b->grid + index * 16 + 14, blocks);
    if (!entry.bytes || unsigned(blocks) * 8 != entry.bytes ||
        word(m, b->grid + index * 16 + 4) != (0xb0000000u | entry.source)) return false;
    bytes = b->rom.subspan(entry.source, entry.bytes);
    if (identity) *identity = b;
    return true;
}
bool bind_replay(unsigned char* m, std::span<const std::uint8_t> rom) {
    replay_bank.reset();replay_bank_epoch=rr64::prediction::replay_epoch;
    if (!rr64::prediction::active() || !m) return false;
    if (!enabled(m)) return true;
    auto candidate=std::make_shared<Bank>();
    candidate->mapping=m;candidate->rom=rom;candidate->grid=word(m,globals::terrain_cell_grid);
    if (!valid_guest_range(candidate->grid,cells*16)) return false;
    // The allocation header is captured in low RDRAM. Walking that image's
    // native pool makes saved cases independent of today's live allocation.
    unsigned p=word(m,0x800bbd00);
    for (unsigned count=0;count<131072;++count) {
        if (!valid_guest_range(p,8) || (p&7)) return false;
        const unsigned size=word(m,p);
        std::uint8_t busy=0,pad=0;
        std::uint16_t alignment=0;
        read_u8(m,p+6,busy);read_u8(m,p+7,pad);read_u16(m,p+4,alignment);
        if (!size) break;
        const unsigned payload=p+8u+pad;
        if (!alignment || alignment>256 || (alignment&(alignment-1)) ||
            pad>=alignment || (payload&(alignment-1)) || (size&7) ||
            !valid_guest_range(p,8u+pad+size)) return false;
        const unsigned capacity=word(m,payload+4);
        if (busy && capacity && capacity<=512u*1024u && size>=capacity+payload_offset &&
            word(m,payload)==magic &&
            word(m,payload+8)==candidate->grid &&
            word(m,payload+20)==rom.size() &&
            word(m,payload+descriptor_offset)==payload+payload_offset) {
            candidate->header=word(m,payload+16);
            candidate->object_grid=word(m,payload+28);
            if (!inspect(*candidate) || candidate->capacity!=capacity ||
                !inspect_objects(*candidate) || size<candidate->allocation_bytes() ||
                word(m,payload+24)!=candidate->allocation_bytes() ||
                (candidate->object_grid && word(m,object_grid_global)!=candidate->object_grid)) return false;
            for (unsigned i=0;i<cells;++i) {
                if (!terrain_source_matches(*candidate,m,i) ||
                    !object_source_matches(*candidate,m,i)) return false;
            }
            candidate->allocation=payload;
            // Never trust a copied last-cell cache against a different ROM
            // supplied to a saved case. Reload from this transaction's ROM.
            write_u32(m,payload+12,no_cell);
            write_u32(m,candidate->collision_header(),no_cell);
            write_u32(m,candidate->collision_header()+4,no_cell);
            replay_bank=std::move(candidate);
            return true;
        }
        p=payload+size;
    }
    // Online and offline race captures require their own saved allocation;
    // an old image without it must not borrow live scratch or silently restore
    // camera-dependent floors.
    return false;
}
}

extern "C" int rr64_online_terrain_prepare(unsigned char* m, void* opaque) noexcept(false) {
    if (!m || !opaque || !enabled(m)) return 1;
    const auto current = current_bank();
    if (current && valid(*current,m) &&
        (rr64::prediction::active() || live_source(*current))) return 1;
    // Historical replay may use only the scratch allocation present when its
    // snapshot was captured. Never allocate from or consult a live heap here.
    if (rr64::prediction::active()) return 0;
    const auto unavailable = []() -> int {
        // Returning false enters online authority failure handling and skips
        // the update. An offline race has no authority session to fail, so
        // report a race error before actor physics rather than wait forever
        // or continue with silently absent ground.
        if (!rr64::prediction::physics_rules().active)
            throw std::runtime_error("Offline race: collision terrain scratch unavailable");
        return 0;
    };
    auto prepared = std::make_shared<Bank>();
    prepared->mapping=m;prepared->rom=recomp::get_rom();
    prepared->header=source_header();
    prepared->grid=word(m,globals::terrain_cell_grid);
    prepared->object_grid=word(m,object_grid_global);
    if (!valid_guest_range(prepared->grid,cells*16) || !inspect(*prepared) ||
        !inspect_objects(*prepared)) return unavailable();
    for (unsigned i=0;i<cells;++i)
        if (!terrain_source_matches(*prepared,m,i) ||
            !object_source_matches(*prepared,m,i)) return unavailable();
    if (!can_allocate(m,prepared->allocation_bytes())) return unavailable();
    auto context=*static_cast<recomp_context*>(opaque);
    context.f_odd=&context.f0.u32h;context.r4=0;context.r5=prepared->allocation_bytes();
    func_8001BDF8(m,&context);
    prepared->allocation=unsigned(context.r2);
    if (!valid_guest_range(prepared->allocation,prepared->allocation_bytes())) return unavailable();
    const unsigned a=prepared->allocation;
    write_u32(m,a,magic);write_u32(m,a+4,prepared->capacity);write_u32(m,a+8,prepared->grid);
    write_u32(m,a+12,no_cell);
    write_u32(m,a+16,prepared->header);write_u32(m,a+20,unsigned(prepared->rom.size()));
    write_u32(m,a+24,prepared->allocation_bytes());write_u32(m,a+28,prepared->object_grid);
    write_u32(m,a+descriptor_offset,a+payload_offset);
    write_u32(m,prepared->collision_header(),no_cell);
    write_u32(m,prepared->collision_header()+4,no_cell);
    bank.store(std::move(prepared),std::memory_order_release);
    return 1;
}

extern "C" void rr64_online_terrain_query_begin(unsigned char* m, void* opaque) {
    if (!m || !opaque) return;
    const auto b=current_bank();
    if (!b || !valid(*b,m)) return;
    const auto& c=*static_cast<recomp_context*>(opaque);
    const unsigned query=unsigned(c.r4), payload=b->allocation+payload_offset;
    if (valid_guest_range(query,0x6c) &&
        (word(m,query+0x2c)==payload || word(m,query+0x30)==payload)) clear_caches(m,query);
}

extern "C" void rr64_online_terrain_lookup(unsigned char* m, void* opaque) {
    if (!m || !opaque || !enabled(m)) return;
    auto& c=*static_cast<recomp_context*>(opaque);
    if (c.r5==5) return;
    const auto b=current_bank();
    if (!b || !valid(*b,m)) return;
    const unsigned record=unsigned(c.r4),query=unsigned(c.r8);
    if (record<b->grid || (record-b->grid)%16 || (record-b->grid)/16>=cells ||
        !valid_guest_range(query,0x6c)) return;
    const unsigned index=(record-b->grid)/16;
    const auto entry=b->entries[index];
    // A genuinely empty cell stays empty. In-flight renderer state is never
    // changed and its DMA may finish independently after this query.
    if (!entry.bytes || word(m,record+4)!=(0xb0000000u|entry.source)) return;
    const unsigned a=b->allocation,payload=a+payload_offset;
    if (word(m,a+12)!=index) {
        for (unsigned i=0;i<entry.bytes;++i) m[((payload-kRdramBegin)+i)^3u]=b->rom[entry.source+i];
        write_u32(m,a+12,index);
    }
    clear_caches(m,query);
    c.r4=guest_address(a+descriptor_offset);c.r5=5;
}

extern "C" void rr64_online_terrain_collision_begin(unsigned char* m, void* opaque) noexcept(false) {
    if (!m || !opaque || !enabled(m)) return;
    auto& c=*static_cast<recomp_context*>(opaque);
    const auto b=current_bank();
    if (!b || !valid(*b,m) || (!rr64::prediction::active() && !live_source(*b)))
        throw rr64::online_terrain::CollisionUnavailable("Static collision bank unavailable");
    const unsigned h=b->collision_header();
    // No scope restoration is needed: renderer grids never change. Clear the
    // prior bucket even when the new one uses the untouched resident path.
    write_u32(m,h+4,no_cell);
    const unsigned offset=unsigned(c.r4);
    if ((offset&15) || offset/16>=cells || unsigned(c.r3)!=b->grid+offset) return;
    const unsigned cell=offset/16;
    // New static fallback is stock-only. Imported courses retain their own
    // terrain/hazard policy and never acquire a stock building in an empty slot.
    if (!b->stock_cells[cell]) return;
    const auto terrain=b->entries[cell], objects=b->objects[cell];
    const bool resident=c.r5==5;
    const unsigned native=word(m,b->object_grid+offset);
    if (resident && (native || !objects.bytes)) return;
    if (!terrain.bytes) return;
    if (!terrain_source_matches(*b,m,cell) || !object_source_matches(*b,m,cell))
        throw rr64::online_terrain::CollisionUnavailable("Static collision cell source changed");
    if (native && (!objects.bytes || !valid_guest_range(native,objects.bytes)))
        throw rr64::online_terrain::CollisionUnavailable("Static collision placement pointer invalid");
    if (word(m,h)!=cell) {
        copy_cell(m,b->collision_terrain(),*b,terrain);
        copy_cell(m,b->collision_objects(),*b,objects);
        write_u32(m,h,cell);
    }
    write_u32(m,h+4,cell);
    write_u32(m,h+8,resident ? 0 : 1);
    // Native62594 uses precisely the original subcell lists, shape math,
    // forces and crash callbacks. Its separate floor-query scratch can change
    // inside a callback without invalidating these wall/placement pointers.
    c.r5=5;
}

namespace {
std::shared_ptr<const Bank> collision_bucket(unsigned char* m, const recomp_context& c) {
    const auto b=current_bank();
    if (!b || !valid(*b,m) || !valid_guest_range(unsigned(c.r29)+0x50,4)) return {};
    const unsigned offset=word(m,unsigned(c.r29)+0x50);
    if ((offset&15) || offset/16>=cells || word(m,b->collision_header()+4)!=offset/16) return {};
    return b;
}
}

extern "C" void rr64_online_terrain_collision_objects(unsigned char* m, void* opaque) {
    if (!m || !opaque || !enabled(m)) return;
    auto& c=*static_cast<recomp_context*>(opaque);
    const auto b=collision_bucket(m,c);
    if (!b) return;
    const unsigned cell=word(m,b->collision_header()+4);
    // Existing native placements take precedence. The absent stock cell is
    // immutable input only; dynamic/breakable entities retain their own list.
    if (!c.r2 && b->objects[cell].bytes) c.r2=guest_address(b->collision_objects());
}

extern "C" void rr64_online_terrain_collision_walls(unsigned char* m, void* opaque) {
    if (!m || !opaque || !enabled(m)) return;
    auto& c=*static_cast<recomp_context*>(opaque);
    if (const auto b=collision_bucket(m,c); b && word(m,b->collision_header()+8))
        c.r19=guest_address(b->collision_terrain());
}
