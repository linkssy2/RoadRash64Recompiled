#include "rr64_mk64_item_hud.hpp"
#include "rr64_course_roulette.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_mk64_item_render.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

namespace rr64::mk64_items {
namespace {
using namespace engine;
constexpr unsigned capacity = 96, record_bytes = 76;
constexpr unsigned counts = 0x800BC9D0, pool = 0x800BCAD8;
struct Sprite {
    unsigned char *mapping = nullptr;
    unsigned record = 0, view = 0, buffer = 0, epoch = 0, sprite = 0, asset = 0;
    unsigned expected_count = 0;
    HudDisplay display{};
    std::array<unsigned char, record_bytes> identity{};
    bool consumed = false;
};
struct Queue {
    Sprite pending{};
    std::array<Sprite, 4> views{};
};
thread_local Queue queue;
struct Focus {
    unsigned char *mapping = nullptr;
    unsigned rider = 0, revision = 0, clock = 0;
    Item held = Item::None;
    bool native = false, clock_known = false;
};
// Native gameplay and HUD creation run on separate guest/host threads. Only
// this small presentation state crosses that boundary; sprite queues do not.
std::mutex focus_mutex;
std::array<Focus, racer_capacity> focus;
std::array<CycleBinding, 4> cycle_bindings;
struct CycleInput {
    std::uint16_t previous = 0, suppressed = 0;
};
std::array<CycleInput, 4> cycle_inputs;
bool same_rider(const Focus &state, const CycleBinding &binding) {
    return state.mapping == binding.mapping && state.rider == binding.rider;
}
void bind_focus(Focus &state, unsigned char *mapping, unsigned rider, const RiderState &item) {
    if (state.mapping != mapping || state.rider != rider || state.held != item.held ||
        state.revision != item.revision) {
        state = {};
        state.mapping = mapping;
        state.rider = rider;
        state.held = item.held;
        state.revision = item.revision;
    }
}
unsigned word(unsigned char *m, unsigned address) {
    unsigned result = 0;
    read_u32(m, address, result);
    return result;
}
unsigned half(unsigned char *m, unsigned address) {
    std::uint16_t result = 0;
    read_u16(m, address, result);
    return result;
}
} // namespace

HudDisplay hud_display(const netplay::CourseWeaponRoll &roll, unsigned clock, unsigned slot,
                       Item held, bool mk64_enabled) noexcept {
    if (!mk64_enabled || slot >= racer_capacity || !valid_item(held))
        return {};
    const bool active = roll.phase && roll.phase <= 2 && clock >= roll.start_clock &&
                        netplay::valid_course_reward(roll.weapon);
    const unsigned age = active ? clock - roll.start_clock : 0;
    if (active && roll.phase == 1) {
        const unsigned reward = course_items::roulette_reward(roll, clock, slot);
        return is_reward(reward) ? HudDisplay{true, reward_item(reward)} : HudDisplay{};
    }
    const bool transient =
        active &&
        (roll.phase == 1 || age < netplay::kCourseRouletteTicks + netplay::kCourseRewardBlinkTicks);
    if (transient && !is_reward(roll.weapon))
        return {};
    if (transient && is_reward(roll.weapon)) {
        const bool blink =
            age >= netplay::kCourseRouletteTicks && ((age - netplay::kCourseRouletteTicks) / 3) % 2;
        return {true, blink ? Item::None : held};
    }
    return {held != Item::None, held};
}
void reset_hud_focus() noexcept {
    const std::lock_guard lock(focus_mutex);
    focus = {};
    cycle_bindings = {};
    // Retain the raw press/consumed-hold latches until physical release. Reset
    // during a held C-Up must not manufacture a new native cycle edge.
}
void focus_native_weapon(unsigned char *mapping, unsigned canonical, unsigned rider,
                         unsigned weapon, const RiderState &item, bool wrapped) noexcept {
    if (canonical >= focus.size())
        return;
    const std::lock_guard lock(focus_mutex);
    auto &state = focus[canonical];
    if (!mapping || !valid_guest_range(rider, engine::rider::stride) || weapon > 14 ||
        !valid_item(item.held) || item.held == Item::None) {
        state = {};
        return;
    }
    if (state.mapping != mapping || state.rider != rider)
        state = {};
    state.mapping = mapping;
    state.rider = rider;
    state.held = item.held;
    state.revision = item.revision;
    state.native = !wrapped;
}
HudDisplay hud_display_for_rider(unsigned char *mapping, unsigned canonical, unsigned rider,
                                 const RiderState &item, const netplay::CourseWeaponRoll &roll,
                                 unsigned clock, unsigned equipped, bool mk64_enabled) noexcept {
    const auto display = hud_display(roll, clock, canonical, item.held, mk64_enabled);
    if (canonical >= focus.size())
        return display;
    const std::lock_guard lock(focus_mutex);
    auto &state = focus[canonical];
    if (!mk64_enabled || !mapping || !valid_guest_range(rider, engine::rider::stride) ||
        equipped > 14 || !valid_item(item.held)) {
        state = {};
        return {};
    }
    if (state.mapping != mapping || state.rider != rider || state.held != item.held ||
        state.revision != item.revision || (state.clock_known && clock < state.clock)) {
        state = {};
        state.mapping = mapping;
        state.rider = rider;
        state.held = item.held;
        state.revision = item.revision;
    }
    state.clock = clock;
    state.clock_known = true;
    // A pickup still spins in the shared square. After the spin, an explicit
    // native switch also dismisses the MK award blink, not just its held icon.
    if (roll.phase == 1 && clock >= roll.start_clock && netplay::valid_course_reward(roll.weapon))
        return display;
    if (item.held == Item::None || state.native)
        return {};
    return display;
}
void publish_cycle_bindings(const std::array<CycleBinding, 4> &bindings) noexcept {
    const std::lock_guard lock(focus_mutex);
    for (unsigned profile = 0; profile < bindings.size(); ++profile) {
        const auto &old = cycle_bindings[profile];
        const auto &next = bindings[profile];
        if (old.canonical < focus.size() && same_rider(focus[old.canonical], old) &&
            (old.mapping != next.mapping || old.canonical != next.canonical ||
             old.rider != next.rider))
            focus[old.canonical] = {};
    }
    cycle_bindings = bindings;
    for (auto &binding : cycle_bindings) {
        if (!binding.mapping || binding.canonical >= focus.size() || !binding.mask ||
            binding.mask > 0xffff || !valid_guest_range(binding.rider, engine::rider::stride) ||
            !valid_item(binding.item.held)) {
            binding = {};
            continue;
        }
        bind_focus(focus[binding.canonical], binding.mapping, binding.rider, binding.item);
    }
}
std::uint16_t filter_cycle_input(unsigned profile, std::uint16_t raw_buttons, std::uint16_t buttons,
                                 bool gameplay_allowed) noexcept {
    if (profile >= cycle_inputs.size())
        return buttons;
    const std::lock_guard lock(focus_mutex);
    auto &input = cycle_inputs[profile];
    const auto pressed = std::uint16_t(raw_buttons & ~input.previous);
    input.previous = raw_buttons;
    input.suppressed &= raw_buttons;
    const auto &binding = cycle_bindings[profile];
    if (gameplay_allowed && binding.mapping && binding.canonical < focus.size() &&
        (pressed & buttons & binding.mask)) {
        auto &state = focus[binding.canonical];
        if (same_rider(state, binding) && state.held != Item::None && !state.native) {
            // Reveal the already equipped native weapon. Host authority and
            // private replay receive the same filtered input, without a cycle.
            state.native = true;
            input.suppressed |= std::uint16_t(buttons & binding.mask);
        }
    }
    return std::uint16_t(buttons & ~input.suppressed);
}
void reset_hud_queue() noexcept {
    queue = {};
}
void arm_hud_sprite(unsigned char *m, unsigned view, unsigned sprite, HudDisplay display) noexcept {
    queue.pending = {};
    if (!m || view >= queue.views.size() || !display.owns || !valid_item(display.shown) ||
        sprite < 0xA3 || sprite > 0xB1 || !render_asset_available())
        return;
    const unsigned buffer = word(m, 0x8009CBA4), asset = word(m, 0x800D6870);
    if (buffer > 1 || !valid_guest_range(asset, 28))
        return;
    const unsigned count = half(m, counts + buffer * 2);
    if (count >= capacity)
        return;
    auto &pending = queue.pending;
    pending.mapping = m;
    pending.record = pool + (buffer * capacity + count) * record_bytes;
    pending.buffer = buffer;
    pending.epoch = word(m, 0x800A1830);
    pending.asset = asset;
    pending.sprite = sprite;
    pending.view = view;
    pending.display = display;
    pending.expected_count = count + 1;
}
void commit_hud_sprite(unsigned char *m) noexcept {
    auto pending = queue.pending;
    queue.pending = {};
    if (!m || pending.mapping != m || !pending.record || word(m, 0x8009CBA4) != pending.buffer ||
        word(m, 0x800A1830) != pending.epoch ||
        half(m, counts + pending.buffer * 2) != pending.expected_count ||
        word(m, pending.record) != pending.asset ||
        half(m, pending.record + 0x10) != pending.sprite || half(m, pending.record + 0x12) != 0 ||
        half(m, pending.record + 0x48) != 0x400)
        return;
    // 30220 supplies caller +40=0 (draw flags) and +44=0x400 (placement).
    // 1EB90 stores them at +12 and +48 respectively. Authenticate the actual
    // original weapon call rather than a synthetic centered-flags record.
    std::memcpy(pending.identity.data(), m + pending.record - 0x80000000u, record_bytes);
    queue.views[pending.view] = pending;
}
} // namespace rr64::mk64_items

extern "C" int rr64_mk64_item_hud_draw_record(unsigned char *m, unsigned record) {
    using namespace rr64::mk64_items;
    using namespace rr64::engine;
    if (!m)
        return 0;
    for (auto &sprite : queue.views) {
        if (sprite.mapping != m || sprite.record != record ||
            word(m, 0x8009CBA4) != sprite.buffer || word(m, 0x800A1830) != sprite.epoch ||
            std::memcmp(sprite.identity.data(), m + record - 0x80000000u, record_bytes))
            continue;
        if (sprite.consumed)
            return 1;
        if (sprite.display.shown == Item::None) {
            sprite.consumed = true;
            return 1; // Blink off suppresses only this original weapon icon.
        }
        const std::uint64_t descriptor =
            std::uint64_t(sprite.asset) + word(m, sprite.asset) + sprite.sprite * 24u;
        if (descriptor > std::numeric_limits<unsigned>::max() ||
            !valid_guest_range(unsigned(descriptor), 24))
            return 0;
        const unsigned width = half(m, unsigned(descriptor) + 10);
        const unsigned height = half(m, unsigned(descriptor) + 8);
        float x = 0, y = 0, sx = 0, sy = 0;
        if (!width || !height || width > 4096 || height > 4096 ||
            !read_float(m, record + 0x14, x) || !read_float(m, record + 0x18, y) ||
            !read_float(m, record + 0x20, sx) || !read_float(m, record + 0x24, sy) ||
            !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(sx) || !std::isfinite(sy))
            return 0;
        x += std::int16_t(half(m, record + 0x44));
        y += std::int16_t(half(m, record + 0x46));
        const float w = width * sx, h = height * sy;
        if (x < -1024 || x > 2048 || y < -1024 || y > 2048 || !(w >= 1 && w <= 512) ||
            !(h >= 1 && h <= 512))
            return 0;
        // Match 1CFB8's quarter-pixel truncation, even when a texture mod has
        // odd dimensions or the native framebuffer scales X and Y differently.
        const int left = int(x * 4) - int((width & ~1u) * 2.f * sx);
        const int top = int(y * 4) - int((height & ~1u) * 2.f * sy);
        const int right = int(float(left) + w * 4);
        const int bottom = int(float(top) + h * 4);
        if (!draw_hud_rectangle(m, sprite.view, sprite.display.shown, left * .25f, top * .25f,
                                (right - left) * .25f, (bottom - top) * .25f))
            return 0;
        sprite.consumed = true;
        return 1;
    }
    return 0;
}
