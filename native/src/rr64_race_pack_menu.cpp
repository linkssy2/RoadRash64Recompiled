#include "rr64_race_pack_menu.hpp"
#include "rr64_local_race_options.hpp"
#include "rr64_thrash_options.hpp"

#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include "rr64_race_pack.hpp"
#include "librecomp/addresses.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>

extern "C" void rr64_course_progress_log(const char *format, ...);

namespace {
namespace catalogue = rr64::race_pack;
using rr64::engine::guest_address;
namespace menu = rr64::engine::local_race;
constexpr unsigned level = 0x800A6690, race = 0x800A6680;
constexpr unsigned solo_level = 0x8009ED18, level_records = 0x8009ECB8;
constexpr unsigned game_type = 0x8009EAE4, unlocks = 0x800A5334;
// The terrain/scenery caches can move later preview allocations well beyond
// 32 MiB. Extended GBI preserves these addresses across the entire guest heap.
constexpr std::size_t guest_limit = recomp::mem_size;
struct Frame {
    unsigned char *owner = nullptr;
    bool active = false;
    std::optional<std::size_t> preview;
};
thread_local Frame frame;
struct PreviewStorage {
    unsigned char *owner = nullptr;
    std::vector<unsigned> addresses;
};
thread_local PreviewStorage images;
unsigned word(unsigned char *rdram, unsigned address) {
    return MEM_W(0, guest_address(address));
}
void word(unsigned char *rdram, unsigned address, unsigned value) {
    MEM_W(0, guest_address(address)) = value;
}
bool progress_enabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("RR64_COURSE_DIAGNOSTICS");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}
void menu_progress(unsigned char *rdram, const char *phase, unsigned detail = 0) {
    if (!progress_enabled() || !rdram) return;
    // A process-wide budget per guest thread, never reset by menu re-entry.
    static thread_local unsigned reports = 0;
    if (reports >= 128) return;
    ++reports;
    const auto index = catalogue::selected_course();
    const auto entries = catalogue::menu_courses();
    const auto id = index && *index < entries.size() ? entries[*index].course_id : std::string_view("stock");
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    rr64_course_progress_log("[RR64-COURSE-PROGRESS] phase=%s time-ns=%lld mode=%u pending=%u selected=%d course=%.*s level=%u race=%u row=%u detail=%u sample=%u\n",
             phase, static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()),
             word(rdram, rr64::engine::globals::main_mode), word(rdram, rr64::engine::globals::pending_mode),
             index ? static_cast<int>(*index) : -1, static_cast<int>(id.size()), id.data(),
             word(rdram, level), word(rdram, race), word(rdram, menu::menu_cursor), detail, reports);
}
bool available(unsigned char *rdram, unsigned native_level) {
    return native_level < 15 && MEM_HU(0, guest_address(unlocks + native_level * 2)) != 0;
}
const catalogue::CourseMenuEntry *selected() {
    const auto entries = catalogue::menu_courses();
    const auto index = catalogue::selected_course();
    return index && *index < entries.size() ? &entries[*index] : nullptr;
}
unsigned multiplayer_base(unsigned char *rdram) {
    const auto type = word(rdram, game_type);
    return type == 1 || type == 5 ? 0 : 8;
}
bool set_carrier(unsigned char *rdram, bool multiplayer,
                 const catalogue::CourseMenuEntry &entry) {
    // Ordinary stock carriers only: special levels also encode unrelated bike
    // and rule modes. The course loader replaces route data at race setup.
    if (entry.safe_stock_level > 4) return false;
    unsigned cursor = entry.safe_stock_level;
    const unsigned native_level = cursor + (multiplayer ? multiplayer_base(rdram) : 0);
    if (!available(rdram, native_level)) return false;
    const unsigned count = word(rdram, 0x800A7420 + native_level * 4);
    if (!count || entry.safe_stock_race >= count) return false;
    if (!multiplayer) {
        for (cursor = 0; cursor < 8; ++cursor)
            if (word(rdram, level_records + cursor * 12) == native_level) break;
        if (cursor == 8) return false;
    }
    const unsigned cursor_address = multiplayer ? level : solo_level;
    const unsigned value = multiplayer ? native_level : cursor;
    if (word(rdram, cursor_address) != value || word(rdram, level) != native_level ||
        word(rdram, race) != entry.safe_stock_race) {
        word(rdram, cursor_address, value);
        word(rdram, race, entry.safe_stock_race);
        word(rdram, menu::menu_dirty, 1);
    }
    return true;
}
struct Choice { bool imported; std::size_t value; };
std::vector<Choice> level_choices(unsigned char *rdram, bool multiplayer) {
    std::vector<Choice> choices;
    const unsigned base = multiplayer ? multiplayer_base(rdram) : 0;
    for (unsigned i = 0; i < (multiplayer ? 7u : 8u); ++i) {
        const unsigned id = multiplayer ? base + i : word(rdram, level_records + i * 12);
        if (available(rdram, id)) choices.push_back({false, multiplayer ? id : i});
    }
    const auto entries = catalogue::menu_courses();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        bool seen = false;
        for (const auto &choice : choices)
            if (choice.imported && entries[choice.value].group_id == entries[i].group_id) seen = true;
        if (!seen) choices.push_back({true, i});
    }
    return choices;
}
std::size_t step(std::size_t index, std::size_t count, bool right) {
    return right ? (index + 1) % count : (index + count - 1) % count;
}
void label(unsigned char *rdram, unsigned buffer, std::string_view text) {
    // Both native selectors reserve exactly 24 bytes before their next local.
    const auto count = std::min<std::size_t>(23, text.size());
    for (unsigned i = 0; i < count; ++i)
        MEM_B(i, guest_address(buffer)) = text[i];
    MEM_B(count, guest_address(buffer)) = 0;
}
unsigned preview_data(unsigned char *rdram, std::size_t index,
                      const catalogue::CourseMenuEntry &entry) {
    if (images.owner != rdram) images = {rdram, {}};
    if (images.addresses.size() != catalogue::menu_courses().size())
        images.addresses.resize(catalogue::menu_courses().size());
    if (images.addresses[index]) return images.addresses[index];
    menu_progress(rdram, "preview-allocate-begin", static_cast<unsigned>(index));
    auto *allocation = static_cast<unsigned char *>(recomp::alloc(rdram, entry.preview_rgba16_be.size()));
    if (!allocation) { menu_progress(rdram, "preview-allocate-failed"); return 0; }
    const auto offset = allocation - rdram;
    if (offset < 0 || std::size_t(offset) > guest_limit - entry.preview_rgba16_be.size()) {
        recomp::free(rdram, allocation);
        menu_progress(rdram, "preview-allocate-invalid");
        return 0;
    }
    const unsigned address = 0x80000000u + unsigned(offset);
    for (std::size_t i = 0; i < entry.preview_rgba16_be.size(); i += 2)
        MEM_H(i, guest_address(address)) = (unsigned(entry.preview_rgba16_be[i]) << 8) |
                                         entry.preview_rgba16_be[i + 1];
    // Immutable per-course guest storage: the next selection cannot overwrite
    // pixels still consumed asynchronously by the preceding RSP task.
    images.addresses[index] = address;
    menu_progress(rdram, "preview-allocate-end", address);
    return address;
}
} // namespace
#endif

extern "C" void rr64_race_pack_menu_reset_session(void) {
    rr64::local_race_options::show_mk64_items_row(false);
#ifdef RR64_EXPERIMENTAL_COURSE
    frame = {};
    images = {};
#endif
}

extern "C" void rr64_race_pack_menu_begin(unsigned char *rdram) {
    rr64::local_race_options::show_mk64_items_row(false);
#ifdef RR64_EXPERIMENTAL_COURSE
    frame = {rdram, false, {}};
#endif
}

extern "C" int rr64_race_pack_menu_input(unsigned char *rdram, unsigned multiplayer) {
#ifdef RR64_EXPERIMENTAL_COURSE
    if (frame.owner != rdram || catalogue::menu_courses().empty()) return 0;
    frame.active = true;
    if (progress_enabled()) {
        static thread_local unsigned last_mode = ~0u;
        const unsigned mode = word(rdram, rr64::engine::globals::main_mode);
        if (last_mode != mode) {
            last_mode = mode;
            menu_progress(rdram, "selector-enter", multiplayer);
        }
    }
    const auto status = rr64::netplay::get_status();
    if (const auto *entry = selected()) set_carrier(rdram, multiplayer != 0, *entry);
    // Online clients display the host's catalogue selection. Their controller
    // never changes it, even if stale input reaches a route-selection frame.
    const unsigned row = word(rdram, menu::menu_cursor);
    const unsigned buttons = word(rdram, menu::menu_buttons);
    if (status.active && !status.is_host) return row < 2 && (buttons & 0x60) ? 1 : 0;
    if (row > 1 || !(buttons & 0x60)) return 0;
    const bool right = (buttons & 0x40) != 0;
    const auto entries = catalogue::menu_courses();
    const auto current = catalogue::selected_course();
    if (row == 1) {
        if (!current || *current >= entries.size()) return 0;
        std::vector<std::size_t> courses;
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].group_id == entries[*current].group_id) courses.push_back(i);
        const auto found = std::find(courses.begin(), courses.end(), *current);
        const auto next = courses[step(std::size_t(found - courses.begin()), courses.size(), right)];
        if (catalogue::select_course(next)) {
            set_carrier(rdram, multiplayer != 0, entries[next]);
            word(rdram, menu::menu_dirty, 1);
            menu_progress(rdram, "race-selected", multiplayer);
        }
        return 1;
    }
    const auto choices = level_choices(rdram, multiplayer != 0);
    if (choices.empty()) return 0;
    const unsigned native_cursor = word(rdram, multiplayer ? level : solo_level);
    std::size_t position = 0;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].imported) {
            if (current && *current < entries.size() &&
                entries[choices[i].value].group_id == entries[*current].group_id) position = i;
        } else if (!current && choices[i].value == native_cursor) position = i;
    }
    const auto next = choices[step(position, choices.size(), right)];
    if (next.imported) {
        if (catalogue::select_course(next.value)) set_carrier(rdram, multiplayer != 0, entries[next.value]);
    } else {
        catalogue::select_stock();
        word(rdram, multiplayer ? level : solo_level, unsigned(next.value));
        if (current) word(rdram, race, 0);
    }
    word(rdram, menu::menu_dirty, 1);
    menu_progress(rdram, "level-selected", multiplayer);
    return 1;
#else
    return 0;
#endif
}

extern "C" int rr64_race_pack_menu_options_input(unsigned char *rdram, unsigned multiplayer) {
    const int course_handled = rr64_race_pack_menu_input(rdram,multiplayer);
#ifdef RR64_EXPERIMENTAL_COURSE
    const unsigned buttons = word(rdram,menu::menu_buttons);
    const unsigned row = word(rdram,menu::menu_cursor);
    rr64::local_race_options::show_mk64_items_row(
        frame.owner == rdram && frame.active && selected() &&
        rr64::local_race_options::course_music_bit(selected()->course_id));
    const bool blocked = frame.owner==rdram && frame.active && selected() &&
                         (row==5 || row==6 || row==7 || row==10);
    if (blocked && row == 7 && (buttons & 0x60u)) {
        // Imported courses have no stock pedestrian population. Reuse that
        // row for the course's music choice, without extending the menu into
        // the footer or changing any stock-course option.
        rr64::local_race_options::toggle_course_music(selected()->course_id);
        word(rdram, menu::menu_dirty, 1);
    }
    if (blocked && row == 6 && (buttons & 0x60u) &&
        rr64::local_race_options::course_music_bit(selected()->course_id)) {
        rr64::local_race_options::toggle_mk64_items(selected()->course_id);
        word(rdram, menu::menu_dirty, 1);
    }
    // Imported courses lack the native traffic/pedestrian/police road graph.
    // Let the normal options helper initialize its table/restore preferences,
    // while suppressing only edits to choices that cannot affect this course.
    if(blocked)word(rdram,menu::menu_buttons,buttons & ~0x60u);
#endif
    const int options_handled = multiplayer ? rr64_local_options_input(rdram)
                                            : rr64_thrash_options_input(rdram);
#ifdef RR64_EXPERIMENTAL_COURSE
    if(blocked)word(rdram,menu::menu_buttons,buttons);
    return course_handled || options_handled || blocked;
#else
    return course_handled || options_handled;
#endif
}

extern "C" void rr64_race_pack_menu_text(unsigned char *rdram, unsigned row, unsigned buffer) {
#ifdef RR64_EXPERIMENTAL_COURSE
    if (frame.owner != rdram || !frame.active) return;
    if (const auto *entry = selected()) {
        if(row<2)label(rdram,buffer,row ? entry->course_name : entry->group_name);
        // The Options heading has an unused value column; this does not add a
        // footer that could overlap the online Ready or Custom Cop reminder.
        else if(row==2)label(rdram,buffer,"Forward only");
        else if(row==5 || row==10)label(rdram,buffer,"AI Cops: Unavailable");
        else if(row==6)label(rdram,buffer,
            rr64::local_race_options::course_music_bit(entry->course_id)
                ? (rr64::local_race_options::mk64_items_enabled()
                    ? "MK64 Items: On" : "MK64 Items: Off")
                : "Traffic: Unavailable");
        else if(row==7)label(rdram,buffer,
            rr64::local_race_options::course_music_enabled(entry->course_id)
                ? "MK64 Music: On" : "MK64 Music: Off");
    }
#endif
}
extern "C" void rr64_race_pack_menu_end(unsigned char *rdram) {
#ifdef RR64_EXPERIMENTAL_COURSE
    if (frame.owner == rdram) {
        frame.active = false;
        rr64::local_race_options::show_mk64_items_row(false);
    }
#endif
}
extern "C" int rr64_race_pack_menu_preview(unsigned char *rdram) {
#ifdef RR64_EXPERIMENTAL_COURSE
    if (frame.owner != rdram || !frame.active || !selected()) return 0;
    // The options sheet remains plain and readable. The picture replaces the
    // stock map only on the Level/Race selector, never character selection.
    if (word(rdram, menu::menu_cursor) < 2) frame.preview = catalogue::selected_course();
    return 1;
#else
    return 0;
#endif
}
extern "C" void rr64_race_pack_menu_draw(unsigned char *rdram) {
#ifdef RR64_EXPERIMENTAL_COURSE
    if (frame.owner != rdram || !frame.preview) return;
    const auto index = *frame.preview;
    frame.preview.reset();
    const auto entries = catalogue::menu_courses();
    if (index >= entries.size()) return;
    const auto &entry = entries[index];
    if (entry.preview_width != 128 || entry.preview_height != 78 ||
        entry.preview_rgba16_be.size() != 128 * 78 * 2) return;
    const unsigned width = word(rdram, 0x800B0808), height = word(rdram, 0x800B080C);
    const auto buffer = word(rdram, 0x8009CBA4), commands = word(rdram, 0x800BC9A0);
    if (!width || width > 1023 || !height || height > 1023 || buffer >= 2 || commands > 0x10000) return;
    const auto base = word(rdram, 0x800AC658 + buffer * 4);
    unsigned dl = word(rdram, 0x800AC650);
    const std::uint64_t end = std::uint64_t(base) + 0x140 + std::uint64_t(commands) * 8;
    // All five strips plus complete state setup fit in 640 bytes. Preserve a
    // generous reserve for native font rendering after this optional picture.
    if (base < 0x80000000 || end > 0x80000000ull + guest_limit ||
        dl < base + 0x148 || (dl & 7) || std::uint64_t(dl) + 8192 > end) return;
    const auto pixels = preview_data(rdram, index, entry);
    if (!pixels) return;
    const auto emit = [&](unsigned a, unsigned b) {
        word(rdram, dl, a); word(rdram, dl + 4, b); dl += 8;
    };
    emit(0xE7000000, 0);           // PipeSync
    // Preview allocations live in the recomp heap above 16 MiB. RT64 must
    // interpret their full guest addresses, not legacy segmented/24-bit ones.
    emit(0xE0525464, 0x10000064);  // gEXEnable
    emit(0x6400002C, 1);           // gEXSetRDRAMExtended(true)
    emit(0xE3000A01, 0);           // One-cycle
    emit(0xE3000C00, 0);           // Texture perspective off
    emit(0xE3001001, 0);           // Direct RGBA16, no preceding CI palette
    emit(0xE3000D01, 0);           // Texture detail clamp
    emit(0xE3000F00, 0);           // Tile LOD
    emit(0xE3001201, 0);           // Nearest sampling, retaining pixel art
    emit(0xE3001402, 0xC00);       // Texture conversion = filter
    emit(0xE2001E01, 0);           // Alpha compare off
    emit(0xE200001C, 0x00504240);  // Translucent 2D surface, no depth writes
    // Donor previews are whole pictures, not cutout sprites: many authored
    // black pixels have alpha zero (2,777 in Rainbow Road). Keep their RGB
    // and the readability tint, but use primitive alpha so the gray native
    // menu cannot show through as holes. G_CC_MODULATEI_PRIM in both cycles.
    emit(0xFC11FE23, 0xFFFFF7FB);  // TEXEL0 * PRIMITIVE RGB; PRIMITIVE alpha
    emit(0xFA000000, 0x929292FF);  // Keep yellow/white native menu text readable
    const float sx = (244.0f / 128) * (float(width) / 320);
    const float sy = (244.0f / 128) * (float(height) / 240);
    const unsigned left = unsigned(std::lround(38.0f * width / 320 * 4));
    const unsigned right = unsigned(std::lround(282.0f * width / 320 * 4));
    for (unsigned y = 0; y < 78; y += 16) {
        const unsigned rows = std::min(16u, 78 - y);
        emit(0xFD10007F, pixels + y * 256); // Keep RT64's extended-address bit31
        emit(0xF5100000, 0x07000000); // Load tile
        emit(0xE6000000, 0);
        emit(0xF3000000, 0x07000000 | ((128 * rows - 1) << 12) | 64);
        emit(0xE7000000, 0);
        emit(0xF5104000, 0x00080200); // 128px line; clamp S/T
        emit(0xF2000000, (127 * 4 << 12) | ((rows - 1) * 4));
        const unsigned top = unsigned(std::lround((49.0f * height / 240 + y * sy) * 4));
        const unsigned bottom = unsigned(std::lround((49.0f * height / 240 + (y + rows) * sy) * 4));
        emit(0xE4000000 | (right << 12) | bottom, (left << 12) | top);
        emit(0xE1000000, 0);
        emit(0xF1000000, (unsigned(std::lround(1024 / sx)) << 16) | unsigned(std::lround(1024 / sy)));
    }
    emit(0xE7000000, 0);
    emit(0x6400002C, 0);           // Restore ordinary addressing for native text
    emit(0xE0525464, 0x20000000);  // gEXDisable
    // The following native text renderer establishes its own combine, filter,
    // palette and colors. No viewport, projection or scissor is changed here.
    word(rdram, 0x800AC650, dl);
#endif
}
