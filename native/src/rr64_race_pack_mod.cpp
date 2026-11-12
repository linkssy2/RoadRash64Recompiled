#include "rr64_race_pack_mod.hpp"
#include "rr64_netplay.hpp"
#include "ultramodern/ultramodern.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <stdexcept>

namespace rr64::race_pack_mod {
namespace {
std::mutex settings_mutex;
std::filesystem::path default_root;
std::atomic_bool enabled{true};
std::atomic_bool started{false};
std::atomic_bool session_enabled{true};
std::atomic_bool files_present{false};
std::atomic_bool importing{false};

std::filesystem::path resolve_root() {
    if (const char *override_root = std::getenv("RR64_PRIVATE_COURSE_PACK");
        override_root && *override_root)
        return std::filesystem::path(override_root);
    return default_root;
}
bool catalogue_present(const std::filesystem::path &root) {
    if (root.empty()) return false;
    std::error_code error;
    return std::filesystem::is_regular_file(root / "catalogue.json", error) && !error;
}
} // namespace

void configure_directory(std::filesystem::path directory) {
    std::scoped_lock lock(settings_mutex);
    if (started.load(std::memory_order_acquire) || importing.load(std::memory_order_acquire)) return;
    default_root = std::move(directory);
    files_present.store(catalogue_present(resolve_root()), std::memory_order_release);
}

bool requested() noexcept { return enabled.load(std::memory_order_acquire); }

bool can_change() noexcept {
    return !importing.load(std::memory_order_acquire) &&
           !started.load(std::memory_order_acquire) && !ultramodern::is_game_started() &&
           !netplay::get_physics_rules().active;
}

bool begin_import(std::filesystem::path& destination) {
#ifndef RR64_EXPERIMENTAL_COURSE
    return false;
#else
    std::scoped_lock lock(settings_mutex);
    if (!can_change()) return false;
    destination = resolve_root();
    if (destination.empty()) return false;
    importing.store(true, std::memory_order_release);
    return true;
#endif
}

void end_import() noexcept {
    std::scoped_lock lock(settings_mutex);
    files_present.store(catalogue_present(resolve_root()), std::memory_order_release);
    importing.store(false, std::memory_order_release);
}

bool set_enabled(bool value) noexcept {
    std::scoped_lock lock(settings_mutex);
    if (!can_change()) return false;
    enabled.store(value, std::memory_order_release);
    return true;
}

Availability availability() noexcept {
#ifndef RR64_EXPERIMENTAL_COURSE
    return Availability::Unsupported;
#else
    if (!files_present.load(std::memory_order_acquire)) return Availability::Missing;
    return requested() ? Availability::Enabled : Availability::Disabled;
#endif
}

std::filesystem::path begin_session() {
    std::scoped_lock lock(settings_mutex);
    // The launcher guard normally prevents this. Keep the lifetime boundary
    // safe if another future start path bypasses the frontend.
    if (importing.load(std::memory_order_acquire))
        throw std::runtime_error("Finish or cancel the MK64 import before starting the game.");
    started.store(true, std::memory_order_release);
    session_enabled.store(requested(), std::memory_order_release);
    // Disabled imports must not touch or validate their asset payloads. Missing
    // files are optional content, so stock races can still initialize normally.
    if (!requested()) return {};
    auto root = resolve_root();
    const bool present = catalogue_present(root);
    files_present.store(present, std::memory_order_release);
    return present ? root : std::filesystem::path{};
}

bool enabled_for_session() noexcept {
    return started.load(std::memory_order_acquire)
        ? session_enabled.load(std::memory_order_acquire) : requested();
}

void abort_session() noexcept {
    std::scoped_lock lock(settings_mutex);
    started.store(false, std::memory_order_release);
}
} // namespace rr64::race_pack_mod
