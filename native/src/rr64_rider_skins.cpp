#include "rr64_rider_skins.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

namespace rr64::rider_skins {
namespace {
using Catalog = std::vector<Appearance>;
std::mutex lifecycle;
std::unique_ptr<const Catalog> resident;
std::atomic<const Catalog *> active{};
std::atomic_bool started{}, random_cpu{};
std::atomic<std::uint64_t> race_seed{};
std::array<std::atomic<unsigned>, 4> menus{};
std::array<std::atomic<unsigned>, 14> racers{};
std::string message = "Enable before starting the game. Local human skins and optional randomized CPU skins.";
bool valid_text(const std::string &text, const char *allowed) {
    return !text.empty() && text.size() <= 32 &&
           text.find_first_not_of(allowed) == std::string::npos;
}
bool same(const Appearance &a, const Appearance &b) {
    if (a.id != b.id || a.name != b.name || a.donor != b.donor ||
        a.turtle_shell != b.turtle_shell || a.dual_head != b.dual_head)
        return false;
    for (unsigned i = 0; i < 4; ++i)
        if (a.textures[i].width != b.textures[i].width ||
            a.textures[i].height != b.textures[i].height ||
            a.textures[i].bytes != b.textures[i].bytes)
            return false;
    return true;
}
template <std::size_t N>
unsigned get(const std::array<std::atomic<unsigned>, N> &values, unsigned slot) {
    const unsigned v = slot < N ? values[slot].load(std::memory_order_acquire) : 0;
    return v <= count() ? v : 0;
}
template <std::size_t N>
void set(std::array<std::atomic<unsigned>, N> &values, unsigned slot, unsigned value) {
    if (slot < N)
        values[slot].store(value <= count() ? value : 0, std::memory_order_release);
}
}
bool install_catalog(std::span<const Appearance> catalog) {
    std::lock_guard lock(lifecycle);
    if (started.load(std::memory_order_acquire) || active.load(std::memory_order_acquire))
        return false;
    auto fail = [&](const char *why) {
        message = why;
        return false;
    };
    if (catalog.empty() || catalog.size() > maximum_characters)
        return fail("Invalid rider skin catalog size.");
    for (unsigned n = 0; n < catalog.size(); ++n) {
        const auto &a = catalog[n];
        if (!valid_text(a.id, "abcdefghijklmnopqrstuvwxyz0123456789-") ||
            !valid_text(a.name,
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_") ||
            (a.donor != 0 && a.donor != 10) || ((a.turtle_shell || a.dual_head) && a.donor != 0) ||
            (a.turtle_shell && a.dual_head))
            return fail("Invalid rider skin identity or native donor.");
        for (unsigned p = 0; p < n; ++p)
            if (catalog[p].id == a.id || catalog[p].name == a.name)
                return fail("Duplicate rider skin identity.");
        for (unsigned i = 0; i < 4; ++i) {
            const auto &t = a.textures[i];
            const unsigned w = i == 3 ? 32 : 64, h = i == 3 ? 16 : 32, pixels = w * h;
            if (t.width != w || t.height != h || t.bytes.size() != pixels + 512)
                return fail("Rider skin texture dimensions or size are invalid.");
            for (unsigned p = pixels + 1; p < t.bytes.size(); p += 2)
                if (!(t.bytes[p] & 1))
                    return fail("Rider skins must have opaque palettes.");
        }
    }
    if (resident) {
        if (catalog.size() != resident->size() ||
            !std::equal(catalog.begin(), catalog.end(), resident->begin(), same))
            return fail("Skin catalog changed. Restart the app to load it.");
    } else
        resident = std::make_unique<const Catalog>(catalog.begin(), catalog.end());
    // Keep published pointers alive until process shutdown, including disabled
    // catalog rescans. Mod mutation is locked out once a game session begins.
    message = "More Characters: human skins offline/local; CPU skins also online locally. Restart to change this mod.";
    active.store(resident.get(), std::memory_order_release);
    return true;
}
void unavailable(std::string reason) {
    std::lock_guard lock(lifecycle);
    if (started.load(std::memory_order_acquire))
        return;
    active.store(nullptr, std::memory_order_release);
    clear_menu();
    clear_race();
    message = std::move(reason);
}
void prepare_session() noexcept {
    std::lock_guard lock(lifecycle);
    active.store(nullptr, std::memory_order_release);
    clear_menu();
    clear_race();
    started.store(false, std::memory_order_release);
}
void begin_session() noexcept {
    std::lock_guard lock(lifecycle);
    started.store(true, std::memory_order_release);
}
bool can_change() noexcept {
    return !started.load(std::memory_order_acquire);
}
bool enabled() noexcept {
    return active.load(std::memory_order_acquire) != nullptr;
}
unsigned count() noexcept {
    const auto *c = active.load(std::memory_order_acquire);
    return c ? unsigned(c->size()) : 0;
}
const Appearance *appearance(unsigned index) noexcept {
    const auto *c = active.load(std::memory_order_acquire);
    return c && index && index <= c->size() ? &(*c)[index - 1] : nullptr;
}
unsigned native_donor(unsigned index) noexcept {
    const auto *a = appearance(index);
    return a ? a->donor : 0;
}
unsigned find(std::string_view id) noexcept {
    const auto *c = active.load(std::memory_order_acquire);
    if (c)
        for (unsigned i = 0; i < c->size(); ++i)
            if ((*c)[i].id == id)
                return i + 1;
    return 0;
}
const char *name(unsigned index) noexcept {
    const auto *a = appearance(index);
    return a ? a->name.c_str() : "";
}
std::string status() {
    std::lock_guard lock(lifecycle);
    return message;
}
unsigned menu_selection(unsigned slot) noexcept {
    return get(menus, slot);
}
void set_menu_selection(unsigned slot, unsigned value) noexcept {
    set(menus, slot, value);
}
unsigned race_selection(unsigned slot) noexcept {
    return get(racers, slot);
}
void set_race_selection(unsigned slot, unsigned value) noexcept {
    set(racers, slot, value);
}
void clear_menu() noexcept {
    for (auto &v : menus)
        v.store(0, std::memory_order_release);
}
void set_random_cpu(bool value) noexcept {
    random_cpu.store(value, std::memory_order_release);
}
unsigned random_cpu_selection(unsigned racer, unsigned donor) noexcept {
    const auto seed = race_seed.load(std::memory_order_acquire);
    const auto *c = active.load(std::memory_order_acquire);
    if (!seed || !c || racer >= racers.size() || (donor != 0 && donor != 10))
        return 0;
    unsigned compatible = 0;
    for (const auto &a : *c) compatible += a.donor == donor;
    if (!compatible) return 0;
    // Cosmetic hashing never consumes the native simulation's random seed.
    auto value = seed + (racer + 1) * UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    unsigned choice = unsigned(value % (compatible * 2));
    if (choice < compatible) return 0; // Equal chance of a stock or custom look.
    choice -= compatible;
    for (unsigned i = 0; i < c->size(); ++i)
        if ((*c)[i].donor == donor && choice-- == 0) return i + 1;
    return 0;
}
void clear_race() noexcept {
    // Latch the setting so paused edits cannot change an ongoing race/replay.
    race_seed.store(random_cpu.load(std::memory_order_acquire)
        ? std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u : 0,
        std::memory_order_release);
    for (auto &v : racers)
        v.store(0, std::memory_order_release);
}
}
