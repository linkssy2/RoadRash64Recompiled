#include "rr64_rider_skins.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <bit>
#include <cmath>
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
std::atomic<unsigned> cpu_percent{100}, cpu_pool{0}, race_percent{100}, race_pool{0};
std::array<std::unique_ptr<const Appearance>, stock_characters> stock;
std::array<std::atomic<const Appearance *>, stock_characters> stock_active{};
std::uint64_t cosmetic_hash(std::uint64_t v) {
    v = (v ^ (v >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    v = (v ^ (v >> 27)) * UINT64_C(0x94d049bb133111eb);
    return v ^ (v >> 31);
}
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
    if (index > maximum_characters && index <= render_characters)
        return stock_active[index - maximum_characters - 1].load(std::memory_order_acquire);
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
void set_cpu_percent(double value) noexcept {
    cpu_percent.store(std::isfinite(value) ? unsigned(std::clamp(value, 0.0, 100.0) + 0.5) : 100);
}
void set_cpu_pool(unsigned value) noexcept { cpu_pool.store(value <= 2 ? value : 0); }
bool cpu_skins_active() noexcept { return race_seed.load() && race_percent.load(); }
// The user ROM owns the stock textures; keep host copies, never packaged assets.
bool load_stock(std::span<const std::uint8_t> rom) {
    constexpr std::size_t bank = 0xf6c440;
    if (rom.size() < 0x1000000) return false;
    const auto word = [&](std::size_t p) {
        return (unsigned(rom[p]) << 24) | (unsigned(rom[p+1]) << 16) |
               (unsigned(rom[p+2]) << 8) | rom[p+3];
    };
    if (word(bank) != 0x47) return false;
    std::array<Appearance, stock_characters> pending;
    for (unsigned n = 0; n < stock_characters; ++n) {
        const auto identity = 0xa2174u + n * 8;
        const unsigned gender = word(identity) >> 16, skin = word(identity + 4);
        if (gender > 1 || skin > 46) return false;
        auto &a = pending[n]; a.donor = gender ? 0 : 10;
        for (unsigned part = 0; part < 4; ++part) {
            const auto entry = bank + 16 + (skin * 4 + part) * 12;
            const std::size_t p = entry + word(entry), size = word(entry + 4);
            const unsigned w = part == 3 ? 32 : 64, h = part == 3 ? 16 : 32;
            if (size != 64 + w*h + 512 || p > rom.size() || size > rom.size()-p ||
                word(p) != 0x16 || word(p+44) != w || word(p+48) != h || word(p+52) != 8 ||
                (word(p+32) & 65535) != 512 || (word(p+36) & 65535) != w*h ||
                (word(p+36) >> 16 & 0x8004)) return false;
            a.textures[part] = {w, h, {rom.begin()+p+64, rom.begin()+p+size}};
        }
    }
    for (unsigned n = 0; n < stock_characters; ++n) install_stock(n, std::move(pending[n]));
    return true;
}
void install_stock(unsigned native, Appearance a) {
    if (native >= stock_characters || (a.donor != 0 && a.donor != 10)) return;
    std::lock_guard lock(lifecycle);
    if (stock[native]) return;
    stock[native] = std::make_unique<const Appearance>(std::move(a));
    stock_active[native].store(stock[native].get(), std::memory_order_release);
}
unsigned random_cpu_selection(unsigned racer, unsigned donor, unsigned mask) noexcept {
    const auto seed = race_seed.load(std::memory_order_acquire);
    const auto *c = active.load(std::memory_order_acquire);
    if (!seed || racer >= racers.size() || !(mask & (1u << racer)) || (mask >> 14) || (donor != 0 && donor != 10))
        return 0;
    // Rank the complete CPU roster, rather than rolling a coin per draw.
    const auto rank_key = [&](unsigned slot) { return cosmetic_hash(seed + slot); };
    unsigned rank = 0;
    for (unsigned i = 0; i < 14; ++i)
        if ((mask & (1u << i)) && (rank_key(i) < rank_key(racer) ||
            (rank_key(i) == rank_key(racer) && i < racer))) ++rank;
    const unsigned selected = (std::popcount(mask) * race_percent.load() + 50) / 100;
    if (rank >= selected) return 0;
    std::array<unsigned, render_characters> pool{};
    unsigned size = 0;
    const auto category = race_pool.load();
    if (category != 2)
        for (unsigned i = 0; i < stock_characters; ++i) {
            const auto *a = appearance(maximum_characters + i + 1);
            if (a && a->donor == donor) pool[size++] = maximum_characters + i + 1;
        }
    if (category != 1 && c)
        for (unsigned i = 0; i < c->size(); ++i)
            if ((*c)[i].donor == donor) pool[size++] = i + 1;
    return size ? pool[cosmetic_hash(seed ^ (UINT64_C(0x9e3779b97f4a7c15) * (racer+1))) % size] : 0;
}
void clear_race() noexcept {
    // Latch the setting so paused edits cannot change an ongoing race/replay.
    race_percent.store(cpu_percent.load());
    race_pool.store(cpu_pool.load());
    race_seed.store(random_cpu.load(std::memory_order_acquire)
        ? std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u : 0,
        std::memory_order_release);
    for (auto &v : racers)
        v.store(0, std::memory_order_release);
}
}
