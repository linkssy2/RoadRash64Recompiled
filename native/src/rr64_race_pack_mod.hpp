#pragma once

#include <filesystem>

namespace rr64::race_pack_mod {
enum class Availability { Unsupported, Missing, Disabled, Enabled };

// Configure once at application startup. The private testing override remains
// supported; ordinary installations use race-packs/mk64 beside the application.
void configure_directory(std::filesystem::path default_directory);
bool requested() noexcept;
bool can_change() noexcept;
bool set_enabled(bool enabled) noexcept;
Availability availability() noexcept;

// Imports hold a lease until the UI has refreshed the mod entry. This keeps
// guest initialization and an on-disk pack replacement mutually exclusive.
bool begin_import(std::filesystem::path& destination);
void end_import() noexcept;

// Only the native ROM initialization boundary may latch this preference. An
// installed terrain/texture bank must never be removed from live guest memory.
std::filesystem::path begin_session();
// Release the import/toggle lock only when initialization failed before guest execution.
void abort_session() noexcept;
bool enabled_for_session() noexcept;
} // namespace rr64::race_pack_mod
